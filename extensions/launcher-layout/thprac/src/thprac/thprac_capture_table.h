#pragma once

#include "thprac_gui_locale.h"

#include <imgui.h>
#include <algorithm>
#include <format>
#include <string>

namespace THPrac::CaptureTable {

template <typename Options>
void DrawTabs(const char* id, const Options& options, int& selection, int preferred)
{
    const auto find = [&](int value) {
        return std::find_if(options.begin(), options.end(),
            [value](const auto& option) { return option.first == value; });
    };
    const bool reset = find(selection) == options.end();
    if (reset) {
        const auto fallback = find(preferred);
        selection = (fallback == options.end() ? options.front() : *fallback).first;
    }
    if (!ImGui::BeginTabBar(id, ImGuiTabBarFlags_FittingPolicyScroll))
        return;
    for (const auto& [value, label] : options) {
        const auto tab = std::format("{}###{}", label, value);
        const auto flags = reset && value == selection ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem(tab.c_str(), nullptr, flags)) {
            if (!reset)
                selection = value;
            ImGui::EndTabItem();
        }
    }
    ImGui::EndTabBar();
}

inline bool Begin(const char* id, const char* nameHeader)
{
    const auto height = std::min(ImGui::GetTextLineHeightWithSpacing() * 16.0f,
        ImGui::GetIO().DisplaySize.y * 0.35f);
    if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg
            | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY,
            ImVec2(0.0f, height)))
        return false;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(S(TH_STAGE));
    ImGui::TableSetupColumn(nameHeader);
    ImGui::TableSetupColumn(S(THPRAC_INGAMEINFO_06_CAPTURE_TOT));
    ImGui::TableSetupColumn(S(THPRAC_INGAMEINFO_06_CAPTURE_CUR));
    ImGui::TableHeadersRow();
    return true;
}

inline void DrawName(std::string name)
{
    // Some localized spell names contain line breaks for narrow overlays.
    std::replace_if(name.begin(), name.end(), [](char c) { return c == '\n' || c == '\r'; }, ' ');
    ImGui::TextUnformatted(name.c_str());
}

inline void DrawRate(long long captures, long long attempts)
{
    ImGui::Text("%lld/%lld(%.1f%%)", captures, attempts,
        attempts ? 100.0 * captures / attempts : 0.0);
}

}
