#define NOMINMAX
#include "thprac_games_dx8.h"

#include <legacy-dx8/d3d8.h>
#include <objidl.h>
#include <shlwapi.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <string>

namespace Gdiplus {
using std::max;
using std::min;
}
#include <gdiplus.h>

namespace THPrac {
namespace {

IDirect3DTexture8* UploadImage(IDirect3DDevice8* device, Gdiplus::Bitmap& source)
{
    UINT width = source.GetWidth();
    UINT height = source.GetHeight();
    D3DCAPS8 caps {};
    if (!width || !height || FAILED(device->GetDeviceCaps(&caps))
        || !caps.MaxTextureWidth || !caps.MaxTextureHeight)
        return nullptr;

    const UINT maxWidth = caps.MaxTextureWidth;
    const UINT maxHeight = caps.MaxTextureHeight;
    width = std::min(width, maxWidth);
    height = std::min(height, maxHeight);
    if (caps.TextureCaps & D3DPTEXTURECAPS_POW2) {
        width = std::bit_ceil(std::min(width, std::bit_floor(maxWidth)));
        height = std::bit_ceil(std::min(height, std::bit_floor(maxHeight)));
    }
    if (caps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY) {
        width = height = std::min(std::max(width, height), std::min(maxWidth, maxHeight));
    }

    IDirect3DTexture8* texture = nullptr;
    if (FAILED(device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture)))
        return nullptr;

    D3DLOCKED_RECT rect {};
    if (FAILED(texture->LockRect(0, &rect, nullptr, 0))) {
        texture->Release();
        return nullptr;
    }

    Gdiplus::Status result;
    {
        Gdiplus::Bitmap destination(width, height, rect.Pitch, PixelFormat32bppARGB,
            static_cast<BYTE*>(rect.pBits));
        Gdiplus::Graphics graphics(&destination);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        result = graphics.DrawImage(&source, Gdiplus::Rect(0, 0, width, height));
    }
    const HRESULT unlock = texture->UnlockRect(0);
    if (result != Gdiplus::Ok || FAILED(unlock)) {
        texture->Release();
        return nullptr;
    }
    return texture;
}

} // namespace

ImTextureID ReadImage8(DWORD device, LPCSTR fileName, LPCSTR srcData, size_t srcSz)
{
    if (!device)
        return nullptr;

    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok)
        return nullptr;

    auto* d3d = reinterpret_cast<IDirect3DDevice8*>(device);
    IDirect3DTexture8* texture = nullptr;
    if (fileName && GetFileAttributesA(fileName) != INVALID_FILE_ATTRIBUTES) {
        const int length = MultiByteToWideChar(CP_ACP, 0, fileName, -1, nullptr, 0);
        if (length > 0) {
            std::wstring path(length, L'\0');
            if (MultiByteToWideChar(CP_ACP, 0, fileName, -1, path.data(), length)) {
                Gdiplus::Bitmap source(path.c_str());
                texture = UploadImage(d3d, source);
            }
        }
    }
    if (!texture && srcData && srcSz && srcSz <= std::numeric_limits<UINT>::max()) {
        if (IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(srcData), static_cast<UINT>(srcSz))) {
            {
                Gdiplus::Bitmap source(stream);
                texture = UploadImage(d3d, source);
            }
            stream->Release();
        }
    }
    Gdiplus::GdiplusShutdown(token);
    return texture;
}

} // namespace THPrac
