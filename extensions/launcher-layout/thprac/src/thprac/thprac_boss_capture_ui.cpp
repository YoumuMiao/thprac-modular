#include "thprac_boss_capture.h"
#include "thprac_boss_capture_runtime.h"
#include "thprac_capture_table.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace THPrac {
namespace BossCapture {
namespace {

enum LocaleIndex : int {
    kLocaleZh = 0,
    kLocaleEn = 1,
    kLocaleJa = 2,
};

const char* Tr(const char* zh, const char* en, const char* ja)
{
    const int locale = Gui::LocaleGet();
    if (locale == kLocaleZh) return zh;
    if (locale == kLocaleJa) return ja;
    return en;
}

const char* KindName(int kind)
{
    return kind == kSpell ? Tr("符卡", "Spell", "スペル") : Tr("非符", "Non-spell", "通常");
}

std::string GameName(int game)
{
    return std::format("TH{:02}", game);
}

const std::vector<int>& GameList()
{
    static const std::vector<int> games = { 6, 7, 8, 10, 11, 12, 13, 14, 16, 17, 18, 20 };
    return games;
}

std::vector<std::pair<int, std::string>> ShotOptions(int game)
{
    const char* names = nullptr;
    switch (game) {
    case 6: names = S(THPRAC_GAMEROLL_TH06_SHOTTYPES); break;
    case 7: names = S(THPRAC_GAMEROLL_TH07_SHOTTYPES); break;
    case 8: names = S(THPRAC_GAMEROLL_TH08_SHOTTYPES); break;
    case 10: names = S(THPRAC_GAMEROLL_TH10_SHOTTYPES); break;
    case 11: names = S(THPRAC_GAMEROLL_TH11_SHOTTYPES); break;
    case 12: names = S(THPRAC_GAMEROLL_TH12_SHOTTYPES); break;
    case 13: names = S(THPRAC_GAMEROLL_TH13_SHOTTYPES); break;
    case 14: names = S(THPRAC_GAMEROLL_TH14_SHOTTYPES); break;
    case 16: names = S(THPRAC_GAMEROLL_TH16_SHOTTYPES); break;
    case 17: names = S(THPRAC_GAMEROLL_TH17_SHOTTYPES); break;
    case 18: names = S(THPRAC_GAMEROLL_TH18_SHOTTYPES); break;
    case 20: names = S(THPRAC_GAMEROLL_TH20_SHOTTYPES); break;
    }
    std::vector<std::pair<int, std::string>> result;
    if (!names) return result;
    const bool compactNames = game == 8 && Gui::LocaleGet() == Gui::LOCALE_ZH_CN;
    constexpr std::string_view groupSuffix = "组";
    for (int index = 0; *names; ++index) {
        const int nativeShot = game == 16 ? index / 4 * 5 + index % 4 : index;
        auto& label = result.emplace_back(nativeShot, names).second;
        if (compactNames) {
            if (index < 4 && label.ends_with(groupSuffix))
                label.resize(label.size() - groupSuffix.size());
            else if (label == "蕾米莉亚")
                label = "蕾米";
        }
        names += std::strlen(names) + 1;
    }
    // TH16's alternate special-season value is also used by practice/replay.
    if (game == 16) for (int character = 0; character < 4; ++character)
        result.emplace_back(character * 5 + 4, std::format("{} {}", character + 1, Tr("特殊季节", "special season", "特別な季節")));
    return result;
}

using RowKey = std::tuple<int, int, int>; // stage, attack, kind

struct Row {
    Counts total;
    Counts session;
};

std::map<RowKey, Row> CollectRows(const Store::Records& total, const Store::Records& session,
    int game, int difficulty, int shot)
{
    std::map<RowKey, Row> rows;
    for (const auto& row : BossCaptureCatalogRows(game, difficulty, shot))
        rows.try_emplace(RowKey(row.stage, row.attack, row.kind));
    for (const auto& entry : total) {
        const Key& key = entry.first;
        if (key.game != game)
            continue;
        if (key.difficulty != difficulty)
            continue;
        if (!BossCaptureStageMatchesDifficulty(game, key.stage, difficulty))
            continue;
        if (key.shot != shot)
            continue;
        Row& row = rows[RowKey(key.stage, key.attack, key.kind)];
        row.total.attempts += entry.second.attempts;
        row.total.captures += entry.second.captures;
        row.total.timeouts += entry.second.timeouts;
        if (const auto found = session.find(key); found != session.end()) {
            row.session.attempts += found->second.attempts;
            row.session.captures += found->second.captures;
            row.session.timeouts += found->second.timeouts;
        }
    }
    return rows;
}

Counts TotalOf(const std::map<RowKey, Row>& rows, bool session)
{
    Counts counts;
    for (const auto& row : rows) {
        const Counts& value = session ? row.second.session : row.second.total;
        counts.attempts += value.attempts;
        counts.captures += value.captures;
        counts.timeouts += value.timeouts;
    }
    return counts;
}

std::string AttackName(const Runtime& runtime, int game, int stage, int attack, int kind, int difficulty = -1, int shot = -1)
{
    if (game == 7 && stage == 6 && attack == TH07::TH07_ST6_CHAOS && kind == kNonSpell)
        return Tr("六开幕", "Stage 6 Opening", "6面開幕");
    if (game == 10 && stage == 4 && attack == TH10::TH10_ST4_MID1 && kind == kNonSpell)
        return Tr("椛椛和小伙伴", "Momiji and Friends", "椛と仲間たち");
    if (game == 12 && attack == kTh12LatterChapterAttack && kind == kNonSpell) {
        if (stage == 4)
            return Tr("4后半#4", "Stage 4 Second Half #4", "4面後半#4");
        if (stage == 5)
            return Tr("5后半#2", "Stage 5 Second Half #2", "5面後半#2");
    }
    if (NativeBridgeActive()) {
        const int locale = Gui::LocaleGet();
        std::vector<std::string> labels;
        const int firstDiff = difficulty < 0 ? 0 : std::clamp(difficulty, 0, 3);
        const int lastDiff = difficulty < 0 ? 3 : firstDiff;
        const bool split6 = game == 6 && attack >= TH06::TH06_ST4_BOSS1 && attack <= TH06::TH06_ST4_BOSS7;
        const bool split14 = game == 14 && attack >= TH14::TH14_ST4_BOSS1 && attack <= TH14::TH14_ST4_BOSS6;
        const int variants6[] = { TH06::TH06_ST4_RA_BOSS1, TH06::TH06_ST4_RB_BOSS1, TH06::TH06_ST4_MA_BOSS1, TH06::TH06_ST4_MB_BOSS1 };
        const int choices = shot < 0 ? (split6 ? 4 : split14 ? 2 : 1) : 1;
        for (int choice = 0; choice < choices; ++choice) {
            int labelAttack = attack;
            const int selectedShot = shot < 0 ? choice : shot;
            if (split6 && selectedShot >= 0 && selectedShot < 4)
                labelAttack = variants6[selectedShot] + attack - TH06::TH06_ST4_BOSS1;
            if (split14 && selectedShot >= 0 && selectedShot < 6)
                labelAttack = (selectedShot % 2 ? TH14::TH14_ST4B_BOSS1 : TH14::TH14_ST4A_BOSS1) + attack - TH14::TH14_ST4_BOSS1;
            for (int diff = firstDiff; diff <= lastDiff; ++diff) {
                const char* name = nullptr;
                switch (game) {
                case 6: if (labelAttack > 0 && labelAttack < static_cast<int>(_countof(TH06::th_sections_str[0][0]))) name = TH06::th_sections_str[locale][diff][labelAttack]; break;
                case 7: if (labelAttack > 0 && labelAttack < static_cast<int>(_countof(TH07::th_sections_str[0][0]))) name = TH07::th_sections_str[locale][diff][labelAttack]; break;
                case 8: if (labelAttack > 0 && labelAttack < static_cast<int>(_countof(TH08::th_sections_str[0][0]))) name = TH08::th_sections_str[locale][diff][labelAttack]; break;
                case 10: if (labelAttack > 0 && labelAttack < static_cast<int>(_countof(TH10::th_sections_str[0][0]))) name = TH10::th_sections_str[locale][diff][labelAttack]; break;
                case 11: if (labelAttack > 0 && labelAttack < static_cast<int>(_countof(TH11::th_sections_str[0][0]))) name = TH11::th_sections_str[locale][diff][labelAttack]; break;
                case 12: if (labelAttack > 0 && labelAttack < static_cast<int>(_countof(TH12::th_sections_str[0][0]))) name = TH12::th_sections_str[locale][diff][labelAttack]; break;
                case 13: if (labelAttack > 0 && labelAttack < static_cast<int>(_countof(TH13::th_sections_str[0][0]))) name = TH13::th_sections_str[locale][diff][labelAttack]; break;
                case 14: if (labelAttack > 0 && labelAttack < static_cast<int>(_countof(TH14::th_sections_str[0][0]))) name = TH14::th_sections_str[locale][diff][labelAttack]; break;
                case 16: if (labelAttack > 0 && labelAttack < static_cast<int>(_countof(TH16::th_sections_str[0][0]))) name = TH16::th_sections_str[locale][diff][labelAttack]; break;
                case 17: if (labelAttack > 0 && labelAttack < static_cast<int>(_countof(TH17::th_sections_str[0][0]))) name = TH17::th_sections_str[locale][diff][labelAttack]; break;
                case 18: if (labelAttack > 0 && labelAttack < static_cast<int>(_countof(TH18::th_sections_str[0][0]))) name = TH18::th_sections_str[locale][diff][labelAttack]; break;
                case 20: if (labelAttack > 0 && labelAttack < static_cast<int>(_countof(TH20::th_sections_str[0][0]))) name = TH20::th_sections_str[locale][diff][labelAttack]; break;
                }
                if (name && *name && std::find(labels.begin(), labels.end(), name) == labels.end()) labels.emplace_back(name);
            }
        }
        if (!labels.empty()) {
            std::string result = game == 13 && difficulty == 5 ? "Overdrive / " : "";
            for (const auto& label : labels) { if (!result.empty() && result.back() != ' ') result += " / "; result += label; }
            return result;
        }
    }
    static const std::string empty;
    for (const auto& entry : runtime.Names()) {
        const Key& key = entry.first;
        if (key.game == game && key.stage == stage && key.attack == attack && key.kind == kind
            && (difficulty < 0 || key.difficulty == difficulty) && (shot < 0 || key.shot == shot))
            return entry.second;
    }
    return empty;
}

void FormatCounts(const Counts& counts, char* buffer, size_t size)
{
    std::snprintf(buffer, size, "%u/%u(%.1f%%)", counts.captures, counts.attempts,
        Runtime::Rate(counts));
}

// A work is listed as connected only when its game module reports real boss
// attack boundaries; the F11 page must not imply coverage that does not exist.
bool WorkConnected(int game)
{
    const auto& games = GameList();
    return std::find(games.begin(), games.end(), game) != games.end();
}
const char* WorkStatusText(int)
{
    return Tr("尚无统计。Boss 非符或符卡开始后自动记录。Replay 不计入。",
        "No records yet. Boss nonspells and spells are recorded automatically; replays are excluded.",
        "記録はまだありません。Boss通常・スペル開始時に自動記録。リプレイは除外します。");
}
const char* WorkShortStatus(int game)
{
    return WorkConnected(game) ? Tr("非符与符卡", "nonspells and spells", "通常・スペル")
        : Tr("未支持", "unsupported", "未対応");
}

} // namespace

void DrawSettingsSection()
{
    Runtime& runtime = Instance();
    runtime.EnsureLoaded();
    Store& store = runtime.Storage();
    ImGui::Separator();
    if (!ImGui::CollapsingHeader(Tr("Boss 收率", "Boss capture rate", "Boss取得率"),
            ImGuiTreeNodeFlags_DefaultOpen))
        return;

    bool enabled = store.Enabled();
    if (ImGui::Checkbox(Tr("记录非符、符卡与章节收率", "Record nonspell, spell, and chapter capture rates",
            "通常・スペル・チャプターの取得率を記録"),
            &enabled)) {
        store.SetEnabled(enabled);
        BossCaptureCancelTracking();
    }
    ImGui::SameLine();
    bool showHud = store.ShowHud();
    if (ImGui::Checkbox(Tr("在详细信息栏显示收率", "Show rates in the information panel", "詳細情報欄に取得率を表示"), &showHud))
        store.SetShowHud(showHud);

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(Tr(
        "实战与练习合并统计；按作品、难度、机体区分非符、符卡与章节。原生收率存档不改写。",
        "Story and practice share one counter set; rows split by work, difficulty, shot and attack type. Native saves are never modified.",
        "実戦と練習は同じ集計。作品・難易度・機体・攻撃種別ごとに記録。ネイティブの記録は変更しません。"));
    ImGui::PopStyleColor();

    if (ImGui::TreeNode(Tr("逐作适配状态", "Per-work status", "作品別の接続状況"))) {
        for (int work : GameList()) {
            ImGui::BulletText("%s  %s", GameName(work).c_str(), WorkShortStatus(work));
        }
        ImGui::TreePop();
    }

    const auto& context = runtime.GetContext();
    static int game = 0;
    if (!game) game = context.game;
    static int selectedDifficulty = -1;
    static int shot = -1;
    static bool confirmClear = false;

    const auto& games = GameList();
    if (std::find(games.begin(), games.end(), game) == games.end())
        game = games.front();

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    if (ImGui::BeginCombo(Tr("作品", "Work", "作品"), GameName(game).c_str())) {
        for (int candidate : games) {
            const bool selected = candidate == game;
            if (ImGui::Selectable(GameName(candidate).c_str(), selected) && !selected) {
                game = candidate;
                selectedDifficulty = -1;
                shot = -1;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::PushID(game);
    std::vector<std::pair<int,const char*>> difficulties = {
        {0,"Easy"}, {1,"Normal"}, {2,"Hard"}, {3,"Lunatic"}, {4,"Extra"}
    };
    if (game==7) difficulties.push_back({5,"Phantasm"});
    if (game==13) difficulties.push_back({5,"Overdrive"});
    if (game==8) difficulties.push_back({7,"Last Word"});
    const int preferredDifficulty = game == context.game && context.difficulty >= 0 ? context.difficulty : 1;
    CaptureTable::DrawTabs("##capture_difficulty", difficulties, selectedDifficulty, preferredDifficulty);
    const int difficulty = selectedDifficulty;

    auto shots = ShotOptions(game);
    for (const auto& entry : store.Total()) {
        if (entry.first.game != game) continue;
        const int value = entry.first.shot;
        if (std::none_of(shots.begin(), shots.end(), [&](const auto& item) { return item.first == value; }))
            shots.emplace_back(value, std::format("{}", value + 1));
    }
    if (shots.empty())
        shots.emplace_back(0, "1");
    if (game == 8) {
        const auto& style = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * 0.5f, style.FramePadding.y));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(style.ItemInnerSpacing.x * 0.5f, style.ItemInnerSpacing.y));
    }
    CaptureTable::DrawTabs("##capture_shot", shots, shot, game == context.game ? context.shot : 0);
    if (game == 8)
        ImGui::PopStyleVar(2);
    ImGui::PopID();

    const auto rows = CollectRows(store.Total(), store.Session(), game, difficulty, shot);
    if (!WorkConnected(game)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", WorkStatusText(game));
        ImGui::PopStyleColor();
    }
    const Counts total = TotalOf(rows, false);
    const Counts session = TotalOf(rows, true);
    char totalBuffer[64], sessionBuffer[64];
    FormatCounts(total, totalBuffer, sizeof(totalBuffer));
    FormatCounts(session, sessionBuffer, sizeof(sessionBuffer));
    ImGui::Text(Tr("合计 累计 %s　本次 %s", "Total %s   Session %s", "合計 %s　今回 %s"),
        totalBuffer, sessionBuffer);

    ImGui::Checkbox(Tr("允许清空数据", "Allow clearing data", "データ消去を許可"), &confirmClear);
    ImGui::SameLine();
    ImGui::BeginDisabled(!confirmClear);
    if (ImGui::Button(Tr("清空本次", "Clear session", "今回を消去"))) {
        store.ClearSession();
        BossCaptureCancelTracking();
        confirmClear = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("清空全部", "Clear all", "全て消去"))) {
        store.ClearAll();
        BossCaptureCancelTracking();
        store.Save();
        confirmClear = false;
    }
    ImGui::EndDisabled();

    if (rows.empty()) {
        ImGui::TextDisabled("%s", WorkStatusText(game));
        return;
    }

    if (CaptureTable::Begin("##boss_capture_rates", Tr("攻击 / 章节", "Attack / Chapter", "攻撃 / チャプター"))) {
        // Grouped attacks (TH08 Last Word) appear after the numbered stages.
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& row : rows) {
                const int stage = std::get<0>(row.first);
                const bool grouped = stage == kGroupedStage;
                if (grouped != (pass == 1)) continue;
                const int attack = std::get<1>(row.first);
                const int kind = std::get<2>(row.first);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (grouped)
                    ImGui::TextUnformatted(Tr("Last Word", "Last Word", "ラストワード"));
                else if (game == 8 && stage >= 1 && stage <= 9)
                    ImGui::TextUnformatted(S(TH08::TH08_STAGE_SELECT[stage - 1]));
                else
                    ImGui::Text("%d", stage);
                ImGui::TableNextColumn();
                auto name = AttackName(runtime, game, stage, attack, kind, difficulty, shot);
                if (name.empty())
                    name = std::format("{} #{}", KindName(kind), attack);
                CaptureTable::DrawName(std::move(name));
                ImGui::TableNextColumn();
                CaptureTable::DrawRate(row.second.total.captures, row.second.total.attempts);
                ImGui::TableNextColumn();
                CaptureTable::DrawRate(row.second.session.captures, row.second.session.attempts);
            }
        }
        ImGui::EndTable();
    }
}

void DrawFailureSlash(const ImVec2& minimum, const ImVec2& maximum)
{
    if (maximum.x <= minimum.x || maximum.y <= minimum.y) return;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(minimum.x,maximum.y),ImVec2(maximum.x,minimum.y),
        IM_COL32(228,132,120,210),std::max(1.25f,ImGui::GetFontSize()*0.08f));
}

void DrawInfoPanelCapture()
{
    Runtime& runtime = Instance();
    const Store& store = runtime.Storage();
    if (!store.ShowHud() || !store.Enabled() || !runtime.Active()) return;
    const Key& key = runtime.ActiveKey();
    const std::string name = AttackName(runtime,key.game,key.stage,key.attack,key.kind,key.difficulty,key.shot);
    const std::string title = name.empty() ? std::format("{} #{}",KindName(key.kind),key.attack) : name;
    Counts total, session;
    if (auto it=store.Total().find(key);it!=store.Total().end()) total=it->second;
    if (auto it=store.Session().find(key);it!=store.Session().end()) session=it->second;
    const std::string all = std::format("{} {}/{} {:.1f}%",Tr("累计","total","累計"),
        total.captures,total.attempts,Runtime::Rate(total));
    const std::string current = std::format("{} {}/{} {:.1f}%",Tr("本次","session","今回"),
        session.captures,session.attempts,Runtime::Rate(session));
    ImGui::Separator();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::TextWrapped("%s",title.c_str());
    const float gap = ImGui::GetFontSize()*0.8f;
    const bool singleLine = ImGui::CalcTextSize(all.c_str()).x+gap+ImGui::CalcTextSize(current.c_str()).x <= width;
    const ImVec4 green(96.0f/255,214.0f/255,150.0f/255,1.0f);
    ImGui::TextColored(green,"%s",all.c_str());
    if (singleLine) ImGui::SameLine(0.0f,gap);
    ImGui::TextColored(green,"%s",current.c_str());
    if (runtime.Failed()) {
        const ImVec2 end = ImGui::GetItemRectMax();
        DrawFailureSlash(start,ImVec2(start.x+width,end.y));
    }
}

}
}
