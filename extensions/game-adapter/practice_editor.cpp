#include "quick_practice_adapter.h"

namespace THPrac {
#define DECLARE_EDITOR(game) namespace game { int EditQuickPractice(QuickPractice::Edit, QuickPractice::Request&); }
DECLARE_EDITOR(TH06)
DECLARE_EDITOR(TH07)
DECLARE_EDITOR(TH08)
DECLARE_EDITOR(TH10)
DECLARE_EDITOR(TH11)
DECLARE_EDITOR(TH12)
DECLARE_EDITOR(TH13)
DECLARE_EDITOR(TH14)
DECLARE_EDITOR(TH15Stock)
DECLARE_EDITOR(TH16)
DECLARE_EDITOR(TH17)
DECLARE_EDITOR(TH18)
DECLARE_EDITOR(TH20)
#undef DECLARE_EDITOR
}

extern "C" __declspec(dllexport) int __cdecl THPracPracticeEditor(const char* game,
    const THPrac::QuickPractice::EditorContext* context, THPrac::QuickPractice::Edit action,
    THPrac::QuickPractice::Request* request)
{
    using namespace THPrac;
    using namespace THPrac::QuickPractice;
    struct Game {
        const char* id;
        int (*edit)(Edit, Request&);
    };
    static constexpr Game games[] = {
        { "th06", TH06::EditQuickPractice }, { "th07", TH07::EditQuickPractice },
        { "th08", TH08::EditQuickPractice }, { "th10", TH10::EditQuickPractice },
        { "th11", TH11::EditQuickPractice }, { "th12", TH12::EditQuickPractice },
        { "th13", TH13::EditQuickPractice }, { "th14", TH14::EditQuickPractice },
        { "th15", TH15Stock::EditQuickPractice }, { "th16", TH16::EditQuickPractice },
        { "th17", TH17::EditQuickPractice }, { "th18", TH18::EditQuickPractice },
        { "th20", TH20::EditQuickPractice }
    };
    for (const auto& item : games) {
        if (strcmp(game, item.id) != 0) continue;
        if (action == Edit::Probe) return 1;
        if (!context || !request) return 0;
        PreviewScope preview(*context, *request);
        return item.edit(action, *request);
    }
    return 0;
}
