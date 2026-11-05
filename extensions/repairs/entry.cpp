#include "repair_service.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
int __cdecl SupportsGame(const char* id)
{
    return std::strcmp(id, "th06") == 0 || std::strcmp(id, "th07") == 0
        || std::strcmp(id, "th08") == 0 || std::strcmp(id, "th14") == 0;
}
int __cdecl Clip(float left, float top, float right, float bottom, float width, float height, uint32_t* viewport)
{
    if (!viewport || !std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0
        || width > 65535 || height > 65535
        || !std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom)) return 0;
    const auto x1 = std::floor(std::clamp(left, 0.0f, width));
    const auto y1 = std::floor(std::clamp(top, 0.0f, height));
    const auto x2 = std::ceil(std::clamp(right, 0.0f, width));
    const auto y2 = std::ceil(std::clamp(bottom, 0.0f, height));
    if (x2 <= x1 || y2 <= y1) return 0;
    viewport[0] = static_cast<uint32_t>(x1);
    viewport[1] = static_cast<uint32_t>(y1);
    viewport[2] = static_cast<uint32_t>(x2 - x1);
    viewport[3] = static_cast<uint32_t>(y2 - y1);
    return 1;
}
const ThpracRepairService repairs {{sizeof(repairs), SupportsGame}, Clip};
const ThpracExtension extension {sizeof(extension), "rueee-c7.repairs", nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, &repairs};
}
extern "C" const ThpracExtension* __cdecl THPracExtensionQuery(uint32_t abi)
{
    return abi == THPRAC_EXTENSION_ABI ? &extension : nullptr;
}
