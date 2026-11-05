#pragma once
#include "thprac_plugin_sdk.h"
#include <optional>

namespace THPrac {
class PluginStageTracker {
public:
    // Null denotes a menu or a restart boundary.
    bool Update(const ThpracPluginStage* stage)
    {
        if (!stage) {
            previous.reset();
            return false;
        }
        if (previous && previous->stage_index == stage->stage_index
            && previous->shot == stage->shot && previous->practice == stage->practice
            && previous->replay == stage->replay) return false;
        previous = *stage;
        return true;
    }

private:
    std::optional<ThpracPluginStage> previous;
};
}
