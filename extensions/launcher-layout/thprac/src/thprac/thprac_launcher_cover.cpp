#define NOMINMAX
#include "thprac_launcher_cover.h"
#include "thprac_launcher_cfg.h"
#include "thprac_launcher_wnd.h"
#include "thprac_utils.h"

#include <Windows.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>

// gdiplus.h calls min/max unqualified from inside its own namespace, so they
// have to be reachable there when NOMINMAX is in effect.
namespace Gdiplus {
    using std::max;
    using std::min;
}
#include <gdiplus.h>

namespace THPrac {
namespace Gui {
    namespace {
        constexpr const wchar_t* COVER_EXTS[] = { L".png", L".jpg", L".jpeg", L".bmp", L".gif" };
        // Anything wider than this gets resized on load: dropping a 4K wallpaper
        // into the folder should not cost tens of megabytes of video memory. The
        // card's art slot is 232x262 layout units, i.e. ~370x420 device pixels at
        // 200% scaling, so 640 still leaves headroom for the Ken Burns zoom while
        // keeping one texture at 1.6 MB instead of 3.2 MB at 900px.
        constexpr int MAX_COVER_EDGE = 640;
        // A missing cover is retried at most this often, so a user can drop a
        // file in while the launcher is open and see it on the next hover.
        constexpr double RESCAN_INTERVAL = 1.0;

        struct CoverEntry {
            void* texture = nullptr;
            ImVec2 size {};
            ImU32 matte = 0; // average colour, used as the letterbox mat
            FILETIME stamp {};
            std::wstring path;
            double nextCheck = 0.0;
        };

        std::unordered_map<std::string, CoverEntry> g_covers;
        std::wstring g_coverFolder;
        bool g_coverFolderReady = false;
        ULONG_PTR g_gdiplusToken = 0;
        bool g_gdiplusReady = false;

        bool EnsureGdiplus()
        {
            if (g_gdiplusReady)
                return true;
            Gdiplus::GdiplusStartupInput input;
            if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) != Gdiplus::Ok)
                return false;
            g_gdiplusReady = true;
            return true;
        }

        bool ReadStamp(const std::wstring& path, FILETIME& stamp)
        {
            WIN32_FILE_ATTRIBUTE_DATA info {};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
                return false;
            if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                return false;
            stamp = info.ftLastWriteTime;
            return true;
        }

        // Uploads a decoded GDI+ image: scales it down to MAX_COVER_EDGE, redraws
        // it into an ARGB32 surface and computes the average colour that tints the
        // card's letterbox mat.
        void* UploadImage(Gdiplus::Image& source, ImVec2& outSize, ImU32& outMatte)
        {
            const int srcWidth = (int)source.GetWidth();
            const int srcHeight = (int)source.GetHeight();
            if (srcWidth <= 0 || srcHeight <= 0)
                return nullptr;

            int width = srcWidth;
            int height = srcHeight;
            const int longest = srcWidth > srcHeight ? srcWidth : srcHeight;
            if (longest > MAX_COVER_EDGE) {
                const float k = (float)MAX_COVER_EDGE / (float)longest;
                width = (int)(srcWidth * k);
                height = (int)(srcHeight * k);
                if (width < 1)
                    width = 1;
                if (height < 1)
                    height = 1;
            }

            // Always redraw into an ARGB32 surface: it normalises palettized,
            // greyscale and CMYK sources into the layout D3D wants.
            Gdiplus::Bitmap target(width, height, PixelFormat32bppARGB);
            if (target.GetLastStatus() != Gdiplus::Ok)
                return nullptr;
            {
                Gdiplus::Graphics graphics(&target);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                graphics.DrawImage(&source, Gdiplus::Rect(0, 0, width, height),
                    0, 0, srcWidth, srcHeight, Gdiplus::UnitPixel);
            }

            Gdiplus::Rect rect(0, 0, width, height);
            Gdiplus::BitmapData data {};
            if (target.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
                return nullptr;
            void* texture = LauncherCreateTextureBGRA(data.Scan0, width, height, data.Stride);

            // Average colour over a coarse grid: cheap, and good enough to tint
            // the letterbox mat behind the artwork.
            uint32_t sumB = 0;
            uint32_t sumG = 0;
            uint32_t sumR = 0;
            uint32_t samples = 0;
            const auto* pixels = (const unsigned char*)data.Scan0;
            const int stepX = (std::max)(1, width / 8);
            const int stepY = (std::max)(1, height / 8);
            for (int y = stepY / 2; y < height; y += stepY) {
                const unsigned char* row = pixels + (ptrdiff_t)y * data.Stride;
                for (int x = stepX / 2; x < width; x += stepX) {
                    const unsigned char* pixel = row + (size_t)x * 4;
                    if (pixel[3] < 8)
                        continue;
                    sumB += pixel[0];
                    sumG += pixel[1];
                    sumR += pixel[2];
                    ++samples;
                }
            }
            target.UnlockBits(&data);

            if (texture) {
                outSize = ImVec2((float)width, (float)height);
                if (samples)
                    outMatte = IM_COL32(sumR / samples, sumG / samples, sumB / samples, 255);
            }
            return texture;
        }

        void* DecodeToTexture(const std::wstring& path, ImVec2& outSize, ImU32& outMatte)
        {
            if (!EnsureGdiplus())
                return nullptr;
            Gdiplus::Bitmap source(path.c_str(), FALSE);
            if (source.GetLastStatus() != Gdiplus::Ok)
                return nullptr;
            return UploadImage(source, outSize, outMatte);
        }
    }

    const wchar_t* LauncherCoverFolder()
    {
        if (!g_coverFolderReady) {
            g_coverFolderReady = true;
            g_coverFolder = LauncherGetDataDir() + L"covers\\";
            // Cheap and idempotent; makes the drop-in folder discoverable
            // instead of leaving the user to guess the path.
            CreateDirectoryW(g_coverFolder.c_str(), nullptr);
        }
        return g_coverFolder.c_str();
    }

    const wchar_t* LauncherCoverLocalFolder()
    {
        static std::wstring folder;
        if (folder.empty()) {
            wchar_t path[MAX_PATH] {};
            GetModuleFileNameW(GetModuleHandleW(nullptr), path, MAX_PATH);
            folder = GetDirFromFullPath(std::wstring(path)) + L"covers\\";
        }
        return folder.c_str();
    }

    // Looks for "<id>.<ext>" in the user's data dir first and next to the
    // executable second, so a portable build can carry its own artwork while a
    // user-supplied file still wins.
    bool FindCover(const std::wstring& gameId, std::wstring& found, FILETIME& stamp)
    {
        const std::wstring folders[] = { LauncherCoverFolder(), LauncherCoverLocalFolder() };
        for (auto& folder : folders) {
            const std::wstring base = folder + gameId;
            for (auto ext : COVER_EXTS) {
                const std::wstring candidate = base + ext;
                if (ReadStamp(candidate, stamp)) {
                    found = candidate;
                    return true;
                }
            }
        }
        found.clear();
        return false;
    }

    bool LauncherCoverGet(const char* gameId, void** texture, ImVec2& pixelSize, ImU32* matte)
    {
        if (texture)
            *texture = nullptr;
        if (matte)
            *matte = 0;
        if (!gameId || !*gameId)
            return false;

        CoverEntry& entry = g_covers[gameId];
        const double now = ImGui::GetTime();
        if (now >= entry.nextCheck) {
            entry.nextCheck = now + RESCAN_INTERVAL;

            const std::wstring wideId = utf8_to_utf16(gameId);
            std::wstring found;
            FILETIME stamp {};
            if (!FindCover(wideId, found, stamp)) {
                LauncherReleaseTexture(entry.texture);
                entry.texture = nullptr;
                entry.matte = 0;
                entry.path.clear();
                entry.size = ImVec2(0.0f, 0.0f);
            } else if (entry.path != found || CompareFileTime(&stamp, &entry.stamp) != 0) {
                ImVec2 decodedSize;
                ImU32 decodedMatte = 0;
                if (void* decoded = DecodeToTexture(found, decodedSize, decodedMatte)) {
                    LauncherReleaseTexture(entry.texture);
                    entry.texture = decoded;
                    entry.matte = decodedMatte;
                    entry.size = decodedSize;
                    entry.stamp = stamp;
                    entry.path = std::move(found);
                }
            }
        }

        if (!entry.texture)
            return false;
        if (texture)
            *texture = entry.texture;
        if (matte)
            *matte = entry.matte;
        pixelSize = entry.size;
        return true;
    }

    void LauncherCoverShutdown()
    {
        for (auto& [id, entry] : g_covers) {
            LauncherReleaseTexture(entry.texture);
            entry.texture = nullptr;
            entry.path.clear();
            entry.matte = 0;
            entry.nextCheck = 0.0;
        }
    }
}
}
