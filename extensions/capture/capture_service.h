#pragma once
#include "thprac_feature_services.h"
#include "thprac_boss_capture.h"

namespace THPrac::BossCapture {
struct CaptureService {
    ThpracFeatureService feature;
    Runtime& (*instance)();
    void (*setNative)(bool);
    bool (*nativeActive)();
    void (*setContext)(Runtime&, const Context&);
    void (*load)(Runtime&);
    bool (*begin)(Runtime&, int, int, int, const std::string&);
    void (*fail)(Runtime&);
    bool (*settle)(Runtime&, Outcome);
    void (*abort)(Runtime&);
    bool (*record)(Runtime&, int, int, int, uint32_t, uint32_t, const std::string&);
    void (*tick)(Runtime&);
    void (*flush)(Runtime&);
    void (*enable)(Store&, bool);
    void (*showHud)(Store&, bool);
    void (*clearSession)(Store&);
    void (*clearTotal)(Store&);
    void (*clearAll)(Store&);
    bool (*valid)(const Key&);
    bool (*save)(Store&);
    bool (*capture)(Store&, const Key&);
    bool (*timeout)(Store&, const Key&);
    void (*log)(const std::string&);
};
}
