#include "thprac_plugin_stage_tracker.h"

int main()
{
    THPrac::PluginStageTracker tracker;
    ThpracPluginStage stage {0, 1, 0, 0};
    if (tracker.Update(nullptr) || !tracker.Update(&stage) || tracker.Update(&stage)) return 1;

    ++stage.stage_index;
    if (!tracker.Update(&stage) || tracker.Update(&stage)) return 2;
    ++stage.shot;
    if (!tracker.Update(&stage)) return 3;
    stage.practice = 1;
    if (!tracker.Update(&stage)) return 4;
    stage.replay = 1;
    if (!tracker.Update(&stage) || tracker.Update(&stage)) return 5;

    if (tracker.Update(nullptr) || !tracker.Update(&stage)) return 6;
    const ThpracPluginStage versus {-1, 0, 0, -1};
    if (!tracker.Update(&versus) || tracker.Update(&versus)) return 7;
    return 0;
}
