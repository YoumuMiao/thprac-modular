#include "route_service.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace {
using namespace THPrac;
int __cdecl SupportsGame(const char* id)
{
    constexpr const char* games[] {
        "th06", "th07", "th08", "th10", "th11", "th12", "th13", "th14", "th16", "th17", "th18", "th20"
    };
    for (const char* game : games)
        if (std::strcmp(id, game) == 0) return 1;
    return 0;
}

RoutePlaybackResult Align(const RoutePlaybackRequest& request)
{
    RoutePlaybackResult result {request.anchor, -1, request.segmentBegin, request.segmentEnd, -1};
    if (request.count <= 0) return result;
    int sample = request.findScript(request.route);
    if (sample < 0 && request.practiceSection == 0)
        sample = request.findClock(request.route, request.currentFrame, true);
    if (sample >= 0 && sample < request.count) {
        result.anchor = {sample, request.currentFrame, true};
        result.scriptSample = sample;
        if (sample < result.segmentBegin || sample >= result.segmentEnd) {
            result.segmentBegin = sample;
            result.segmentEnd = sample + 1;
            const int segment = request.getSegment(request.route, sample);
            while (result.segmentBegin > 0 && request.getSegment(request.route, result.segmentBegin - 1) == segment)
                --result.segmentBegin;
            while (result.segmentEnd < request.count && request.getSegment(request.route, result.segmentEnd) == segment)
                ++result.segmentEnd;
        }
    }
    if (!result.anchor.valid || result.anchor.sample < 0 || result.anchor.sample >= request.count
        || result.segmentBegin >= result.segmentEnd) return result;
    const int64_t clock = static_cast<int64_t>(request.getFrame(request.route, result.anchor.sample))
        + std::max<int64_t>(0, static_cast<int64_t>(request.currentFrame) - result.anchor.clock) + request.trim;
    const auto clampedClock = static_cast<int>(std::clamp<int64_t>(clock,
        std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
    const int predicted = request.findClock(request.route, clampedClock, false);
    result.sample = std::clamp(predicted, result.segmentBegin, result.segmentEnd - 1);
    return result;
}

const RouteService route {{sizeof(route), SupportsGame}, Align};
const ThpracExtension extension {sizeof(extension), "rueee-c7.route", nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, &route};
}
extern "C" const ThpracExtension* __cdecl THPracExtensionQuery(uint32_t abi)
{
    return abi == THPRAC_EXTENSION_ABI ? &extension : nullptr;
}
