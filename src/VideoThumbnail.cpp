#include "neon/VideoThumbnail.hpp"

#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <fstream>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include "neon/Utils.hpp"
#include "neon/ArtworkFileCache.hpp"
#include "neon/VideoSourceCache.hpp"

namespace neon {
namespace {
using Microsoft::WRL::ComPtr;
using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
constexpr DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

bool usefulFrame(SDL_Surface* surface) {
    if (!surface) return false;
    double sum{}, squares{}, count{}, dark{}, light{};
    for (int y = surface->h / 6; y < surface->h * 5 / 6; y += std::max(1, surface->h / 24)) {
        for (int x = surface->w / 6; x < surface->w * 5 / 6; x += std::max(1, surface->w / 32)) {
            Uint8 r{}, g{}, b{}, a{};
            if (!SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a)) continue;
            const double brightness = (r + g + b) / 3.0;
            sum += brightness; squares += brightness * brightness; ++count;
            if (brightness < 15) ++dark;
            if (brightness > 240) ++light;
        }
    }
    return count > 0 && dark / count < 0.95 && light / count < 0.95 &&
           squares / count - (sum / count) * (sum / count) > 90;
}

SDL_Surface* shellThumbnail(const Track& track) {
    Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(track.path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return nullptr;
    HBITMAP bitmap{};
    if (FAILED(factory->GetImage({768, 432},
        static_cast<SIIGBF>(SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK), &bitmap)) || !bitmap) return nullptr;
    struct Bitmap { HBITMAP value; ~Bitmap() { DeleteObject(value); } } ownedBitmap{bitmap};
    BITMAP description{};
    if (!GetObjectW(bitmap, sizeof(description), &description) ||
        description.bmWidth <= 0 || description.bmHeight <= 0 ||
        description.bmWidth > 4096 || description.bmHeight > 4096) return nullptr;
    auto* surface = SDL_CreateSurface(description.bmWidth, description.bmHeight, SDL_PIXELFORMAT_BGRA32);
    if (!surface) return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = surface->w;
    info.bmiHeader.biHeight = -surface->h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const auto dc = GetDC(nullptr);
    const auto rows = dc ? GetDIBits(dc, bitmap, 0, surface->h, surface->pixels, &info, DIB_RGB_COLORS) : 0;
    if (dc) ReleaseDC(nullptr, dc);
    if (rows != surface->h) { SDL_DestroySurface(surface); return nullptr; }
    // RGB DIB alpha bytes are undefined, even when the thumbnail is opaque.
    for (int y = 0; y < surface->h; ++y) {
        auto* row = static_cast<unsigned char*>(surface->pixels) + y * surface->pitch;
        for (int x = 0; x < surface->w; ++x) row[x * 4 + 3] = 255;
    }
    return surface;
}

SDL_Surface* copySample(IMFSourceReader* reader, IMFSample* sample) {
    ComPtr<IMFMediaType> type;
    UINT32 width{}, height{};
    if (FAILED(reader->GetCurrentMediaType(videoStream, &type)) ||
        FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
        width == 0 || height == 0 || width > 8192 || height > 8192) return nullptr;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return nullptr;
    Surface surface(SDL_CreateSurface(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_BGRA32), SDL_DestroySurface);
    if (!surface) return nullptr;
    BYTE* top{};
    LONG pitch{};
    ComPtr<IMF2DBuffer> twoD;
    const bool locked2D = SUCCEEDED(buffer.As(&twoD)) && SUCCEEDED(twoD->Lock2D(&top, &pitch));
    if (!locked2D) {
        DWORD length{};
        UINT32 rawPitch{};
        if (FAILED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawPitch))) {
            if (FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &pitch))) return nullptr;
        } else pitch = static_cast<LONG>(rawPitch);
        if (FAILED(buffer->Lock(&top, nullptr, &length))) return nullptr;
        if (std::abs(static_cast<long long>(pitch)) * (height - 1) + width * 4LL > length) { buffer->Unlock(); return nullptr; }
        if (pitch < 0) top += (height - 1) * static_cast<std::size_t>(-pitch);
    }
    if (std::abs(static_cast<long long>(pitch)) < width * 4LL) {
        if (locked2D) twoD->Unlock2D(); else buffer->Unlock();
        return nullptr;
    }
    for (UINT32 y = 0; y < height; ++y) {
        auto* row = static_cast<unsigned char*>(surface->pixels) + y * surface->pitch;
        std::memcpy(row, top + static_cast<std::ptrdiff_t>(y) * pitch, width * 4);
        for (UINT32 x = 0; x < width; ++x) row[x * 4 + 3] = 255;
    }
    if (locked2D) twoD->Unlock2D(); else buffer->Unlock();
    const float scale = std::min(768.0F / width, 432.0F / height);
    if (scale < 1) {
        auto* smaller = SDL_ScaleSurface(surface.get(), std::max(1, static_cast<int>(width * scale)),
                                         std::max(1, static_cast<int>(height * scale)), SDL_SCALEMODE_LINEAR);
        if (smaller) return smaller;
    }
    return surface.release();
}

SDL_Surface* representativeFrame(const Track& track, std::stop_token stop) {
    if (stop.stop_requested()) return nullptr;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return nullptr;
    struct Runtime { ~Runtime() { MFShutdown(); } } runtime;
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 2))) return nullptr;
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(track.path.c_str(), attributes.Get(), &reader))) return nullptr;
    if (stop.stop_requested()) return nullptr;
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(videoStream, TRUE);
    ComPtr<IMFMediaType> type;
    if (FAILED(MFCreateMediaType(&type))) return nullptr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(reader->SetCurrentMediaType(videoStream, nullptr, type.Get()))) return nullptr;
    const auto duration = track.durationMs > 0 ? track.durationMs : 60000;
    Surface fallback(nullptr, SDL_DestroySurface);
    for (const auto milliseconds : {std::min<std::int64_t>(15000, duration / 10), duration / 3}) {
        if (stop.stop_requested()) return nullptr;
        PROPVARIANT position{};
        position.vt = VT_I8;
        position.hVal.QuadPart = milliseconds * 10000;
        reader->SetCurrentPosition(GUID_NULL, position);
        for (int frame = 0; frame < 12; ++frame) {
            if (stop.stop_requested()) return nullptr;
            ComPtr<IMFSample> sample;
            DWORD flags{};
            if (FAILED(reader->ReadSample(videoStream, 0, nullptr, &flags, nullptr, &sample)) ||
                (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) break;
            if (!sample) continue;
            Surface decoded(copySample(reader.Get(), sample.Get()), SDL_DestroySurface);
            if (usefulFrame(decoded.get())) return decoded.release();
            if (decoded) fallback = std::move(decoded);
        }
    }
    return fallback.release();
}
}

std::filesystem::path videoThumbnailCachePath(const Track& track, const std::filesystem::path& cacheRoot) {
    const auto root = cacheRoot.empty() ? pathFromUtf8(SDL_GetBasePath()) / L"library" / L"video-thumbnails" : cacheRoot;
    const auto key = makeStableId(pathToUtf8(track.path) + "\n" + std::to_string(track.fileSize) +
                                  "\n" + std::to_string(track.modifiedTicks));
    return root / pathFromUtf8(key + ".png");
}

SDL_Surface* loadVideoThumbnail(const Track& track, const std::filesystem::path& cacheRoot, std::stop_token stop) {
    if (stop.stop_requested() || track.mediaKind != MediaKind::Video || track.path.empty()) return nullptr;
    const auto path = videoThumbnailCachePath(track, cacheRoot);
    ArtworkFileLease lease(path, stop);
    if (!lease || stop.stop_requested()) return nullptr;
    std::error_code filesystemError;
    if (std::filesystem::is_regular_file(path, filesystemError)) {
        if (auto* cached = IMG_Load(pathToUtf8(path).c_str())) return cached;
    }
    const auto missing = path.wstring() + L".missing";
    if (std::filesystem::is_regular_file(missing, filesystemError)) return nullptr;
    if (stop.stop_requested()) return nullptr;
    // Thumbnail work must not compete with playback for normal-priority CPU time.
    const auto oldPriority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    struct Priority { int previous; ~Priority() { SetThreadPriority(GetCurrentThread(), previous); } } priority{oldPriority};
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return nullptr;
    struct Apartment {
        bool owned;
        ~Apartment() { if (owned) CoUninitialize(); }
    } apartment{SUCCEEDED(initialized)};
    Track source = track;
    // Reuse an already prepared playback file, without copying whole videos
    // just to obtain a thumbnail or touching a slow/disconnected USB drive.
    const auto playbackCopy = videoSourceCachePath(track, path.parent_path().parent_path() / L"video-media");
    const auto cachedSize = std::filesystem::file_size(playbackCopy, filesystemError);
    if (!filesystemError && track.fileSize > 0 && cachedSize == track.fileSize) source.path = playbackCopy;
    filesystemError.clear();
    if (!std::filesystem::is_regular_file(source.path, filesystemError) || stop.stop_requested()) return nullptr;
    Surface thumbnail(shellThumbnail(source), SDL_DestroySurface);
    if (stop.stop_requested()) return nullptr;
    if (!usefulFrame(thumbnail.get())) {
        if (auto* frame = representativeFrame(source, stop)) thumbnail.reset(frame);
    }
    // A cancelled read says nothing about this video's validity. In particular,
    // never persist a permanent "missing" result for an interrupted extraction.
    if (stop.stop_requested()) return nullptr;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (thumbnail) {
        const auto temporary = path.wstring() + L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
        if (IMG_SavePNG(thumbnail.get(), pathToUtf8(temporary).c_str()))
            MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(temporary.c_str());
    } else {
        std::ofstream marker(std::filesystem::path(missing), std::ios::binary);
        marker << "No thumbnail for this file version\n";
    }
    return thumbnail.release();
}
}
