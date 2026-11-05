#pragma once
#include "thprac_feature_services.h"

struct ThpracRepairService {
    ThpracFeatureService feature;
    int (__cdecl *clip_dx8)(float left, float top, float right, float bottom,
        float display_width, float display_height, uint32_t* viewport);
};
