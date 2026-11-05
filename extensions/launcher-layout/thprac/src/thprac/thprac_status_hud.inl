// Shared TH15-style HUD. Included inside namespace THPrac by thprac_games.cpp.
namespace {
struct StatusHudLayout {
    int game;
    uintptr_t playerGlobal;
    uintptr_t managerGlobal;
    unsigned bossSlots, listHead, position, health, flags;
    uint32_t bossFlag;
};
// Offsets are relative to the whole Enemy, not EnemyData. TH16+ boss slots
// contain handles, so those titles enumerate the active list and test flags.
// The player field is not repeated here: it comes from PlayerPositionOffset in
// thprac_player_offsets.h, the same table the route overlay reads.
static const StatusHudLayout statusHudLayouts[] = {
    {6, 0, 0, 0, 0, 0xc6c, 0xce4, 0, 0},
    {7, 0, 0, 0, 0, 0x2b0c, 0x2bb8, 0, 0},
    {8, 0, 0, 0, 0, 0x2d88, 0x2dfc, 0, 0},
    {10, 0x477834, 0x477704, 0x10, 0, 0x1070, 0x23fc, 0, 0},
    {11, 0x4a8eb4, 0x4a8d7c, 0x1c, 0, 0x1070, 0x252c, 0, 0},
    {12, 0x4b4514, 0x4b43dc, 0x1c, 0, 0x1074, 0x2648, 0, 0},
    {13, 0x4c22c4, 0x4c2188, 0x5c, 0, 0x1230, 0x5138, 0, 0},
    {14, 0x4db67c, 0x4db544, 0x5c, 0, 0x1234, 0x5164, 0, 0},
    {16, 0x4a6ef8, 0x4a6dc0, 0, 0x180, 0x1250, 0x5180, 0x526c, 0x800000},
    {17, 0x4b77d0, 0x4b76a0, 0, 0x180, 0x1250, 0x5180, 0x528c, 0x800000},
    {18, 0x4cf410, 0x4cf2d0, 0, 0x18c, 0x1270, 0x6220, 0x635c, 0x800000},
    // TH20 uses module-relative globals and a sentinel-based scheduler list.
    {20, 0x1ba56c, 0x1ba570, 0, 0x10c, 0x198, 0x214, 0x354, 0x80},
};
static bool statusHudHp = true, statusHudBoss = true, statusHudPlayer = true;
static bool StatusHudPair(uintptr_t address, float& x, float& y) {
    // Check both values before reading; a missing object is never a (0,0) pair.
    MEMORY_BASIC_INFORMATION region{};
    if (!address || !VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))
        || region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        || address + 8 < address
        || address + 8 > reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize) return false;
    x = CaptureRead<float>(address); y = CaptureRead<float>(address + 4);
    return std::isfinite(x) && std::isfinite(y);
}
static int32_t StatusHudPhaseHp(int game, uintptr_t enemy, unsigned offset) {
    const int32_t hp = CaptureRead<int32_t>(enemy + offset);
    int32_t boundary = 0;
    if (game == 6) boundary = CaptureRead<int32_t>(enemy + 0xea8);
    else if (game == 7 || game == 8) {
        const unsigned thresholds = game == 7 ? 0x2ebc : 0x3358;
        for (unsigned i = 0; i < 4; ++i) {
            const int32_t candidate = CaptureRead<int32_t>(enemy + thresholds + i * 4);
            if (candidate > boundary && candidate < hp) boundary = candidate;
        }
    } else boundary = CaptureRead<int32_t>(enemy + offset + 0x10);
    return std::max(0, hp - (boundary > 0 && boundary < hp ? boundary : 0));
}
}

void GameStatusHudOptions() {
    // TH15 retains its existing controls and chapter-aware health reader.
    if (!AddonEnabled(Addon::Hud) || !captureContext) return;
    const int locale = Gui::LocaleGet();
    auto label = [locale](const char* zh, const char* ja, const char* en) {
        return locale == Gui::LOCALE_ZH_CN ? zh : locale == Gui::LOCALE_JA_JP ? ja : en;
    };
    ImGui::Separator();
    ImGui::TextUnformatted(label("游戏状态显示（F8）", "ゲーム状態表示（F8）", "Game status HUD (F8)"));
    ImGui::Checkbox(label("显示 Boss 血量", "ボスHPを表示", "Show boss HP"), &statusHudHp);
    ImGui::Checkbox(label("显示 Boss 坐标", "ボス座標を表示", "Show boss position"), &statusHudBoss);
    ImGui::Checkbox(label("显示自机坐标", "自機座標を表示", "Show player position"), &statusHudPlayer);
}

void GameStatusHudDraw(ImDrawList* p, int game, bool enabled) {
    if (!AddonEnabled(Addon::Hud) || !enabled || (!statusHudHp && !statusHudBoss && !statusHudPlayer)) return;
    const StatusHudLayout* layout = nullptr;
    for (const auto& candidate : statusHudLayouts) if (candidate.game == game) layout = &candidate;
    if (!layout || !captureContext || !captureContext().gameplay) return;
    const auto& l = *layout;
    const uintptr_t base = game == 20 ? reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) : 0;
    float px = 0, py = 0;
    bool playerValid = false;
    if (game < 10) {
        auto readPlayer = RouteProfileFor(game).readPlayer;
        playerValid = readPlayer && readPlayer(px, py);
        px += 192.0f; // early titles store coordinates measured from the left edge
    } else {
        const auto player = CaptureRead<uintptr_t>(base + l.playerGlobal);
        playerValid = player && StatusHudPair(player + THPrac::PlayerPositionOffset(game), px, py);
    }
    if (!playerValid) return;
    uintptr_t boss = 0;
    if (game < 10) {
        const auto context = captureContext();
        for (auto candidate : context.bosses) if (candidate) { boss = candidate; break; }
    } else {
        const auto manager = CaptureRead<uintptr_t>(base + l.managerGlobal);
        if (manager && l.bossSlots) {
            for (unsigned i = 0; i < 4; ++i) {
                boss = CaptureRead<uintptr_t>(manager + l.bossSlots + i * 4);
                if (boss) break;
            }
        } else if (manager) {
            auto node = CaptureRead<uintptr_t>(manager + l.listHead);
            for (unsigned i = 0; node && i < 4096; ++i) {
                const auto enemy = CaptureRead<uintptr_t>(node);
                if (enemy && (CaptureRead<uint32_t>(enemy + l.flags) & l.bossFlag)) { boss = enemy; break; }
                const auto next = CaptureRead<uintptr_t>(node + 4);
                if (next == node) break;
                node = next;
            }
        }
    }
    std::string hpText = "HP --", bossText = "B (--,--)";
    float bx = 0, by = 0;
    if (boss && StatusHudPair(boss + l.position, bx, by)) {
        hpText = std::format("HP {}", StatusHudPhaseHp(game, boss, l.health));
        bossText = std::format("B({:.1f},{:.1f})", bx, by);
    }
    const std::string playerText = std::format("P({:.1f},{:.1f})", px, py);
    const float rowHeight = ImGui::GetTextLineHeight();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    // Match RenderLockTimer: TH06-TH13 use 32..110, TH14+ use 64..220.
    // Fonts already follow the game's GUI scale; scale only horizontal geometry.
    const float horizontalScale = game <= 13 ? 0.5f : 1.0f;
    const float padding = 4.0f * horizontalScale;
    auto drawField = [&](float x, float width, const std::string& value) {
        x *= horizontalScale;
        width *= horizontalScale;
        p->AddRectFilled({x, 0.0f}, {x + width, rowHeight}, 0xFFFFFFFF);
        p->PushClipRect({x, 0.0f}, {x + width, rowHeight}, true);
        const ImVec2 textSize = ImGui::CalcTextSize(value.c_str());
        const float scale = textSize.x > width - 2.0f * padding ? (width - 2.0f * padding) / textSize.x : 1.0f;
        p->AddText(font, fontSize * scale, {x + padding, (rowHeight - textSize.y * scale) * 0.5f}, 0xFF000000, value.c_str());
        p->PopClipRect();
    };
    if (statusHudHp) drawField(224.0f, 132.0f, hpText);
    if (statusHudBoss) drawField(360.0f, 150.0f, bossText);
    if (statusHudPlayer) drawField(514.0f, 176.0f, playerText);
}
