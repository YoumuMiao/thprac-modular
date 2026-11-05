#pragma once
#include "thprac_feature_services.h"
#include "thprac_route_sync.h"

namespace THPrac {
struct RoutePlaybackRequest {
    int count;
    int practiceSection;
    int currentFrame;
    int trim;
    RouteSync::Anchor anchor;
    int segmentBegin;
    int segmentEnd;
    const void* route;
    int (*findScript)(const void*);
    int (*findClock)(const void*, int, bool exact);
    int (*getFrame)(const void*, int);
    int (*getSegment)(const void*, int);
};
struct RoutePlaybackResult {
    RouteSync::Anchor anchor;
    int scriptSample;
    int segmentBegin;
    int segmentEnd;
    int sample;
};
struct RouteService {
    ThpracFeatureService feature;
    RoutePlaybackResult (*align)(const RoutePlaybackRequest&);
};
}
