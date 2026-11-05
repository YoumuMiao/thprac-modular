#pragma once

// Playback math for the route reference.
//
// TH15's overlay does not search the saved path for the player every frame: a
// route is sampled once per game frame, so a sample index is also a frame
// offset, and playback keeps the sample the run starts on together with the
// engine clock at that moment. Everything here is that arithmetic, kept free
// of Windows and ImGui so the regression test can compile it on its own.
// Point types only need .x and .y members.

#include <cmath>
#include <cstdint>
#include <map>
#include <utility>

namespace THPrac {
namespace RouteSync {

// Native ECL location and VM time, independent of the player's movement and
// of the address at which the game happened to load its scripts.
struct ScriptStamp {
    uint32_t script = 0;
    int time = 0;
};

class ScriptIndex {
    std::map<std::pair<uint32_t, int>, int> samples;
public:
    void Clear() { samples.clear(); }
    void Add(const ScriptStamp& stamp, int sample)
    {
        if (!stamp.script || sample < 0) return;
        auto [it, inserted] = samples.emplace(std::make_pair(stamp.script, stamp.time), sample);
        // A frozen timeline or a repeated script is ambiguous. Never use it to
        // guess which occurrence a practice jump belongs to.
        if (!inserted && it->second != sample) it->second = -1;
    }
    int Find(const ScriptStamp& stamp) const
    {
        auto it = samples.find({stamp.script, stamp.time});
        return it == samples.end() ? -1 : it->second;
    }
};

struct FrameGate {
    int previous = -1;
    bool Advance(int frame)
    {
        if (frame < 0 || frame == previous) return false;
        previous = frame;
        return true;
    }
    void Reset() { previous = -1; }
};

// Where playback currently is in the route.
struct Anchor {
    int sample = -1;
    int clock = 0;
    bool valid = false;
};

// A practice target whose route position has been established. TH15 derives
// these from the stage scripts; the shared engine learns them the first time a
// target is played and writes them into the route file.
struct SectionMark {
    int section = 0;
    int sample = 0;
};

// Sample for the current clock, clamped to the route.
inline int ProgressOf(const Anchor& anchor, int clock, int count)
{
    if (!anchor.valid || count <= 0)
        return -1;
    int index = anchor.sample;
    if (clock > anchor.clock)
        index += clock - anchor.clock;
    if (index < 0)
        index = 0;
    if (index >= count)
        index = count - 1;
    return index;
}

template <class Point>
inline float SquaredDistance(const Point& left, const Point& right)
{
    const float dx = left.x - right.x;
    const float dy = left.y - right.y;
    return dx * dx + dy * dy;
}

// Sample of a practice target, or -1 when the route has no mark for it.
inline int SectionSample(const SectionMark* marks, int count, int section)
{
    if (!marks || section <= 0)
        return -1;
    for (int i = 0; i < count; ++i) {
        if (marks[i].section == section)
            return marks[i].sample;
    }
    return -1;
}

// Multi-frame acquisition. A single point is not enough: routes cross
// themselves and the practice spawn sits on the path, so the reference is
// located by matching the last `frames` player positions against the route.
// `trajectory[frames - 1]` is the newest frame, so the result is the route
// sample that corresponds to `trajectory[0]`.
//
// Returns false unless the best match is both close (mean distance within
// `tolerance` pixels) and clearly better than any match starting at least
// `frames` samples away. Otherwise the caller keeps waiting for movement
// instead of locking onto the wrong stretch.
template <class Point>
bool Acquire(const Point* route, int count, const Point* trajectory, int frames,
    int stride, float tolerance, int& sample, float* bestMean = nullptr)
{
    if (bestMean)
        *bestMean = -1.0f;
    if (!route || !trajectory || count <= 0 || frames < 4 || frames > count || stride < 1)
        return false;
    const int last = count - frames;
    int bestStart = -1;
    float best = 0.0f;
    for (int start = 0; start <= last; start += stride) {
        float sum = 0.0f;
        for (int i = 0; i < frames; ++i)
            sum += SquaredDistance(route[start + i], trajectory[i]);
        const float mean = sum / static_cast<float>(frames);
        if (bestStart < 0 || mean < best) {
            best = mean;
            bestStart = start;
        }
    }
    if (bestStart < 0)
        return false;
    float runnerUp = 0.0f;
    bool hasRunnerUp = false;
    for (int start = 0; start <= last; start += stride) {
        if (start > bestStart - frames && start < bestStart + frames)
            continue;
        float sum = 0.0f;
        for (int i = 0; i < frames; ++i)
            sum += SquaredDistance(route[start + i], trajectory[i]);
        const float mean = sum / static_cast<float>(frames);
        if (!hasRunnerUp || mean < runnerUp) {
            runnerUp = mean;
            hasRunnerUp = true;
        }
    }
    const float mean = std::sqrt(best);
    if (bestMean)
        *bestMean = mean;
    if (mean > tolerance)
        return false;
    // A second place that matches almost as well means the route is ambiguous
    // here; wait for more movement instead of locking onto the wrong stretch.
    // The absolute slack keeps two identical stretches from passing as a
    // unique match when the best one is exact.
    if (hasRunnerUp && runnerUp <= best * 2.25f + 4.0f)
        return false;
    sample = bestStart;
    return true;
}

} // namespace RouteSync
} // namespace THPrac
