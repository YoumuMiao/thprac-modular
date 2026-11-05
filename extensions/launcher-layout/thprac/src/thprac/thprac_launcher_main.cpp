#include "thprac_extension_manager.h"
#include "host_apply.h"
#include "thprac_launcher_main.h"
#include "thprac_gui_components.h"
#include "thprac_gui_locale.h"
#include "thprac_launcher_cfg.h"
#include "thprac_launcher_games.h"
#include "thprac_launcher_links.h"
#include "thprac_launcher_tools.h"
#include "thprac_launcher_others.h"
#include "thprac_launcher_keng.h"
#include "thprac_launcher_utils.h"
#include "thprac_launcher_wnd.h"
#include "thprac_utils.h"
#include "thprac_version.h"
#include <ShlObj.h>
#include <Windows.h>
#include <Shlwapi.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <sstream>

#include <thread>
#include <numbers>


namespace THPrac {
extern bool g_isLauncher;
const char* gTabToOpen = nullptr;
LauncherTrigger gLauncherTrigger = LAUNCHER_NOTHING;

namespace {

    // The launcher window is a borderless popup, so every piece of chrome the
    // stock ImGui title bar used to provide is drawn here instead.
    struct LauncherChrome {
        bool drag = false;
        bool minimize = false;
        bool close = false;
    };

    constexpr float LAUNCHER_HEADER_HEIGHT = 56.0f;

    // Tab order, used by the page transition to know which way the user moved.
    enum LauncherTabIndex {
        LAUNCHER_TAB_GAMES = 0,
        LAUNCHER_TAB_OTHERS,
        LAUNCHER_TAB_LINKS,
        LAUNCHER_TAB_TOOLS,
        LAUNCHER_TAB_KENG,
        LAUNCHER_TAB_SETTINGS,
        LAUNCHER_TAB_PLUGINS,
    };

    // Soft disc that follows the pointer inside a small button. The radial
    // falloff is faked by stacking concentric discs: a pixel covered by n discs
    // accumulates n * alpha, and n drops linearly with the distance from the
    // pointer, so the stack reads as a single soft gradient.
    void LauncherDrawPointerGlow(ImDrawList* drawList, ImVec2 minV, ImVec2 size, ImU32 color, float strength)
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImVec2 maxV(minV.x + size.x, minV.y + size.y);
        if (mouse.x < minV.x || mouse.x >= maxV.x || mouse.y < minV.y || mouse.y >= maxV.y)
            return;

        auto clampf = [](float value, float low, float high) {
            return value < low ? low : (value > high ? high : value);
        };
        const ImVec2 center(clampf(mouse.x, minV.x + 1.0f, maxV.x - 1.0f),
                            clampf(mouse.y, minV.y + 1.0f, maxV.y - 1.0f));
        const float radius = (size.x < size.y ? size.x : size.y) * 0.5f;
        const float breathe = 0.80f + 0.20f * std::sin((float)ImGui::GetTime() * 3.0f);
        const int steps = 18;
        const int slice = (int)(clampf(strength * breathe, 0.0f, 1.0f) * 126.0f) / steps;
        if (slice <= 0)
            return;

        const ImU32 base = color & ~IM_COL32_A_MASK;
        drawList->PushClipRect(minV, maxV, true);
        for (int i = steps; i >= 1; --i) {
            const float t = (float)i / (float)steps;
            const int alpha = (int)(slice * (1.0f - 0.55f * t));
            if (alpha <= 0)
                continue;
            drawList->AddCircleFilled(center, radius * t, base | ((ImU32)alpha << IM_COL32_A_SHIFT), 28);
        }
        drawList->PopClipRect();
    }

    void LauncherDrawBackdrop(ImVec2 wndPos, ImVec2 wndSize, float scale)
    {
        const Gui::ChromeColors& chrome = Gui::GetChromeColors();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 br(wndPos.x + wndSize.x, wndPos.y + wndSize.y);

        // Page surface: slightly lighter at the top so the header reads as a
        // lit band rather than a flat plate.
        drawList->AddRectFilledMultiColor(wndPos, br,
            chrome.pageTopLeft, chrome.pageTopRight, chrome.pageBottomRight, chrome.pageBottomLeft);

        // Accent bloom fading out from under the header.
        const float headerH = LAUNCHER_HEADER_HEIGHT * scale;
        const float glowH = 200.0f * scale;
        const ImU32 glowTop = chrome.pageGlow;
        const ImU32 glowHalf = (glowTop & ~IM_COL32_A_MASK)
            | (((glowTop >> IM_COL32_A_SHIFT) / 2) << IM_COL32_A_SHIFT);
        const ImU32 glowNone = glowTop & ~IM_COL32_A_MASK;
        drawList->AddRectFilledMultiColor(ImVec2(wndPos.x, wndPos.y + headerH),
            ImVec2(br.x, wndPos.y + headerH + glowH), glowTop, glowHalf, glowNone, glowNone);

        // 1px accent frame instead of a system border.
        drawList->AddRect(wndPos, ImVec2(br.x - 1.0f, br.y - 1.0f), chrome.pageFrame);
    }

    void LauncherDrawTitleBar(ImVec2 wndPos, ImVec2 wndSize, float scale, LauncherChrome& chrome)
    {
        const Gui::ChromeColors& palette = Gui::GetChromeColors();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float barH = LAUNCHER_HEADER_HEIGHT * scale;
        const ImVec2 p0 = wndPos;
        const ImVec2 p1(wndPos.x + wndSize.x, wndPos.y + barH);

        drawList->AddRectFilledMultiColor(p0, p1,
            palette.headerLeft, palette.headerRight, palette.headerRight, palette.headerLeft);
        // 1px top highlight: keeps the band from reading as a flat fill.
        drawList->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y + 1.0f), palette.headerTopHighlight);
        drawList->AddRectFilled(ImVec2(p0.x, p1.y - 1.0f), ImVec2(p1.x, p1.y), palette.headerRule);

        // Brand mark: the application icon itself (green/white yin-yang). The
        // hand-drawn disc below stays as a fallback for the unlikely case the
        // icon resource cannot be turned into a texture.
        const float markSize = 30.0f * scale;
        const ImVec2 markMin(p0.x + 18.0f * scale, p0.y + (barH - markSize) * 0.5f);
        const ImVec2 markMax(markMin.x + markSize, markMin.y + markSize);
        if (void* iconTexture = LauncherGetIconTexture()) {
            // Faint halo so the light icon keeps its shape on the light header.
            drawList->AddCircleFilled(ImVec2((markMin.x + markMax.x) * 0.5f, (markMin.y + markMax.y) * 0.5f),
                markSize * 0.54f, IM_COL32(28, 157, 91, 28), 32);
            drawList->AddImage(static_cast<ImTextureID>(iconTexture), markMin, markMax);
        } else {
            const float markR = 11.0f * scale;
            const ImVec2 markC(markMin.x + markSize * 0.5f, p0.y + barH * 0.5f);
            drawList->AddCircleFilled(markC, markR + 3.0f * scale, IM_COL32(28, 157, 91, 45), 32);
            drawList->AddCircleFilled(markC, markR, IM_COL32(28, 157, 91, 240), 32);
            drawList->AddCircleFilled(ImVec2(markC.x - markR * 0.34f, markC.y - markR * 0.34f),
                markR * 0.40f, IM_COL32(235, 255, 245, 235), 24);
            drawList->AddCircleFilled(ImVec2(markC.x + markR * 0.40f, markC.y + markR * 0.32f),
                markR * 0.18f, IM_COL32(9, 34, 22, 190), 16);
        }

        // Title: "thprac" in full contrast, the localized tagline dimmed.
        const float lineH = ImGui::GetFontSize();
        const float textY = p0.y + barH * 0.5f - lineH * 0.5f;
        const float textX = markMax.x + 14.0f * scale;
        const char* fullTitle = S(THPRAC_LAUNCHER);
        const char* dash = strstr(fullTitle, " - ");
        if (dash && dash != fullTitle) {
            const std::string brand(fullTitle, static_cast<size_t>(dash - fullTitle));
            drawList->AddText(ImVec2(textX, textY), palette.title, brand.c_str());
            const float brandW = ImGui::CalcTextSize(brand.c_str()).x;
            drawList->AddText(ImVec2(textX + brandW + 10.0f * scale, textY), palette.subtitle, dash + 3);
        } else {
            drawList->AddText(ImVec2(textX, textY), palette.title, fullTitle);
        }

        // Window buttons on the right edge.
        const float btn = 30.0f * scale;
        const float pad = 10.0f * scale;
        const ImVec2 closePos(p1.x - pad - btn, p0.y + (barH - btn) * 0.5f);
        const ImVec2 minPos(closePos.x - btn - 4.0f * scale, closePos.y);

        char versionText[48];
        snprintf(versionText, sizeof(versionText), "v%s", GetVersionStr());
        const ImVec2 versionSize = ImGui::CalcTextSize(versionText);
        drawList->AddText(ImVec2(minPos.x - 16.0f * scale - versionSize.x,
                                  p0.y + barH * 0.5f - versionSize.y * 0.5f),
            palette.version, versionText);

        // Drag zone covers everything left of the buttons. This deliberately
        // uses a hit test instead of an item: an item would capture the active
        // id, and the system move loop started by HTCAPTION never delivers the
        // matching mouse-up.
        ImGui::SetCursorScreenPos(minPos);
        ImGui::InvisibleButton("##launcher_min", ImVec2(btn, btn));
        const bool minHovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            chrome.minimize = true;

        ImGui::SetCursorScreenPos(closePos);
        ImGui::InvisibleButton("##launcher_close", ImVec2(btn, btn));
        const bool closeHovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            chrome.close = true;

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        chrome.drag = mouse.x >= p0.x && mouse.x < minPos.x && mouse.y >= p0.y && mouse.y < p1.y
            && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive();

        const ImVec2 minC(minPos.x + btn * 0.5f, minPos.y + btn * 0.5f);
        if (minHovered) {
            drawList->AddCircleFilled(minC, btn * 0.5f, palette.glyphHoverBackground, 24);
            LauncherDrawPointerGlow(drawList, minPos, ImVec2(btn, btn), palette.glyph, 0.55f);
        }
        const float dashHalf = 5.0f * scale;
        drawList->AddLine(ImVec2(minC.x - dashHalf, minC.y), ImVec2(minC.x + dashHalf, minC.y),
            minHovered ? palette.title : palette.glyph, 1.6f * scale);

        const ImVec2 closeC(closePos.x + btn * 0.5f, closePos.y + btn * 0.5f);
        if (closeHovered) {
            drawList->AddCircleFilled(closeC, btn * 0.5f, palette.closeHoverBackground, 24);
            LauncherDrawPointerGlow(drawList, closePos, ImVec2(btn, btn), IM_COL32(255, 255, 255, 255), 0.6f);
        }
        const float crossHalf = 5.0f * scale;
        const ImU32 crossCol = closeHovered ? IM_COL32(255, 255, 255, 255) : palette.glyph;
        drawList->AddLine(ImVec2(closeC.x - crossHalf, closeC.y - crossHalf),
            ImVec2(closeC.x + crossHalf, closeC.y + crossHalf), crossCol, 1.6f * scale);
        drawList->AddLine(ImVec2(closeC.x - crossHalf, closeC.y + crossHalf),
            ImVec2(closeC.x + crossHalf, closeC.y - crossHalf), crossCol, 1.6f * scale);

        ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    }

    // Rounded panel that hosts the content of one tab. The border flag is what
    // makes ImGui keep WindowPadding for the child (borderless children get
    // their padding forced to zero). `offsetX`/`alpha` are the page transition:
    // the panel slides in from the side of travel while fading up.
    void LauncherTabPanelBegin(const char* id, float scale, float offsetX = 0.0f, float alpha = 1.0f)
    {
        const Gui::ChromeColors& palette = Gui::GetChromeColors();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.cardBackground);
        ImGui::PushStyleColor(ImGuiCol_Border, palette.cardBorder);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * scale);
        ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), true);
    }

    void LauncherTabPanelEnd()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }
}

void GuiLauncherMainSwitchTab(const char* tab)
{
    gTabToOpen = tab;
}
bool GuiTabItem(const char* tabText)
{
    auto flag = 0;
    if (gTabToOpen && !strcmp(gTabToOpen, tabText)) {
        flag = ImGuiTabItemFlags_SetSelected;
        gTabToOpen = nullptr;
    }
    return ImGui::BeginTabItem(tabText, 0, flag);
}
void ErrorMsgBox(th_glossary_t textRef)
{
    auto title = utf8_to_utf16(S(THPRAC_PR_ERROR));
    auto text = utf8_to_utf16(S(textRef));
    MessageBoxW(nullptr, text.c_str(), title.c_str(), MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
}

void GuiLauncherLocaleInit()
{
    if (LauncherCfgInit(true)) {
        if (!Gui::LocaleInitFromCfg()) {
            Gui::LocaleAutoSet();
        }
        LauncherCfgClose();
    }
}
void GuiLauncherMainTrigger(LauncherTrigger trigger)
{
    gLauncherTrigger = trigger;
}

HHOOK g_hKeyboardHook = nullptr;
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode < 0 || nCode != HC_ACTION) // do not process message
        return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);

    bool bEatKeystroke = false;
    auto p = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    switch (wParam) {
    case WM_KEYDOWN:
    case WM_KEYUP: {
        bEatKeystroke =(p->vkCode == VK_LWIN) || (p->vkCode == VK_RWIN);
        // Note that this will not block the Xbox Game Bar hotkeys (Win+G, Win+Alt+R, etc.)
        break;
    }
    }

    if (bEatKeystroke)
        return 1;
    else
        return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

// void DisableWinKey(){
//     if (!g_hKeyboardHook)
//         g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(nullptr), 0);
// }
// 
// void DisableDisableWinKey()
// {
//     if (g_hKeyboardHook){
//         UnhookWindowsHookEx(g_hKeyboardHook);
//         g_hKeyboardHook = nullptr;
//     }
// }
// 

int GuiLauncherMain()
{
    int localeSwitch = -1;


    if (!LauncherCfgInit()) {
        ErrorMsgBox(THPRAC_PR_ERR_LAUNCHER_CFG);
        return -1;
    }

    auto initResult = LauncherWndInit(640, 480, 1280, 960, 960, 720);
    if (initResult <= 0) {
        if (initResult) {
            ErrorMsgBox(THPRAC_PR_ERR_LAUNCHER_INIT);
        }
        return -1;
    }
    InitLocaleAndChore();

    int theme;
    if (LauncherSettingGet("theme", theme)) {
        // LauncherSettingSet doesn't take int, only int&.
        // Passing 0 will call the overload with const char*
        int Sus = 1;
        const char* theme_user = nullptr;
        // LauncherSettingGet only accepts signed ints but I want to do an unsigned comparison
        if ((unsigned int)theme > 2) {
            if (LauncherSettingGet("theme_user", theme_user) && theme_user) {
                std::wstring theme_path = LauncherGetDataDir() + L"themes\\" + utf8_to_utf16(theme_user);
                if (!PathFileExistsW(theme_path.c_str())) {
                    LauncherSettingSet("theme", Sus);
                    theme = Sus;
                }
            } else {
                LauncherSettingSet("theme", Sus);
                theme = Sus;
            }
        }

        if (theme_user) {
            SetTheme(theme, utf8_to_utf16(theme_user).c_str());
        } else {
            SetTheme(theme);
        }
    }
    auto scale = LauncherWndGetScale();
    LauncherOthersInit();
    LauncherKengInit();

    g_hKeyboardHook = nullptr;
    // if (LauncherSettingGet("disable_win_key", disable_win) && disable_win) {
    //     DisableWinKey();
    // }
    bool pluginsWereOpen = false;
    while (LauncherWndNewFrame()) {
        static bool isOpen = true;
        bool canMove = false;
        LauncherOthersUpdate();

        const float headerH = LAUNCHER_HEADER_HEIGHT * scale;

        // Cap the resize grip at the largest size the window can be dragged to
        // (1280x960 at 96 DPI, scaled for the monitor and raised with the UI
        // scale) and open at the resolution picked in the launcher settings.
        // The drag floor stays at the design minimum so a large UI scale does
        // not forbid a small window; the panels scroll instead.
        const ImVec2 maxWndSize = LauncherWndGetMaxSize();
        const float dpiScale = LauncherWndGetDpiScale();

        ImGui::SetNextWindowSizeConstraints(ImVec2(640.0f * dpiScale, 480.0f * dpiScale), maxWndSize);
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(LauncherWndGetStartupSize(), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(1.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("###thprac_wnd", nullptr,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();

        ImVec2 wndPos = ImGui::GetWindowPos();
        ImVec2 wndSize = ImGui::GetWindowSize();

        LauncherDrawBackdrop(wndPos, wndSize, scale);
        LauncherChrome chrome;
        LauncherDrawTitleBar(wndPos, wndSize, scale, chrome);
        if (chrome.close)
            isOpen = false;
        if (chrome.minimize)
            LauncherWndMinimize();
        canMove = chrome.drag;
        if (!isOpen) {
            ImGui::End();
            LauncherWndEndFrame(wndPos, wndSize, false);
            break;
        }

        // The sidebar reaches both horizontal edges; only the content has side margins.
        ImGui::SetCursorScreenPos(ImVec2(wndPos.x, wndPos.y + headerH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 10.0f * scale));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::BeginChild("##launcher_body", ImVec2(wndSize.x, wndSize.y - headerH), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        // Keep navigation visible on wide windows; a labelled selector leaves
        // the entire width to the content on small windows and large UI scales.
        static int activeTab = LAUNCHER_TAB_GAMES;
        const auto locale = Gui::LocaleGet();
        const char* pages[] { S(THPRAC_GAMES), S(THPRAC_OTHERS), S(THPRAC_LINKS),
            S(THPRAC_TOOLS), S(THPRAC_KENG), S(THPRAC_SETTINGS),
            locale == Gui::LOCALE_ZH_CN ? "插件管理" : locale == Gui::LOCALE_JA_JP ? "プラグイン管理" : "Plugins" };
        const int pageCount = static_cast<int>(std::size(pages)) - (HasPluginManager() ? 0 : 1);
        if (gTabToOpen) {
            for (int i = 0; i < pageCount; ++i)
                if (!strcmp(gTabToOpen, pages[i])) activeTab = i;
            gTabToOpen = nullptr;
        }
        const bool wide = ImGui::GetContentRegionAvail().x >= 928.0f * scale;
        const ImVec2 bodyStart = ImGui::GetCursorPos();
        if (wide) {
            ImGui::BeginChild("##navigation", ImVec2(204.0f * scale, 0), false);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(28.0f * scale, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            for (int i = 0; i < pageCount; ++i) {
                if (i == LAUNCHER_TAB_SETTINGS) ImGui::Spacing();
                const bool selected = activeTab == i;
                const auto& palette = Gui::GetChromeColors();
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_Button, selected
                    ? ImGui::GetStyleColorVec4(ImGuiCol_Header) : ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
                ImGui::PushStyleColor(ImGuiCol_Text, selected ? ImGui::ColorConvertU32ToFloat4(palette.sectionTitle) : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                if (ImGui::Button(pages[i], ImVec2(ImGui::GetContentRegionAvail().x, 36.0f * scale)))
                    activeTab = i;
                ImGui::PopStyleColor(4);
                ImGui::PopID();
            }
            ImGui::PopStyleVar(4);
            ImGui::EndChild();
            ImGui::SetCursorPos(ImVec2(bodyStart.x + 204.0f * scale, bodyStart.y));
        } else {
            ImGui::SetCursorPosX(14.0f * scale);
            ImGui::SetNextItemWidth(wndSize.x - 28.0f * scale);
            if (ImGui::BeginCombo("##navigation_compact", pages[activeTab])) {
                for (int i = 0; i < pageCount; ++i) {
                    if (ImGui::Selectable(pages[i], activeTab == i)) activeTab = i;
                    if (activeTab == i) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        if (!wide) ImGui::SetCursorPosX(14.0f * scale);
        ImGui::BeginChild("##page_workspace", ImVec2(ImGui::GetContentRegionAvail().x - 14.0f * scale, 0), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Gui::GetChromeColors().sectionTitle), "%s", pages[activeTab]);
        ImGui::Spacing();
        // Stable content coordinates avoid moving click targets while navigating.
        const float pageOffset = 0.0f;
        const float pageAlpha = 1.0f;

        switch (activeTab) {
        case LAUNCHER_TAB_GAMES:
            LauncherTabPanelBegin("##games", scale, pageOffset, pageAlpha);
            LauncherGamesGuiUpd();
            LauncherTabPanelEnd();
            break;
        case LAUNCHER_TAB_OTHERS:
            LauncherTabPanelBegin("##others", scale, pageOffset, pageAlpha);
            LauncherOthersGuiUpd();
            LauncherTabPanelEnd();
            break;
        case LAUNCHER_TAB_LINKS:
            LauncherTabPanelBegin("##links", scale, pageOffset, pageAlpha);
            LauncherLinksGuiUpd();
            LauncherTabPanelEnd();
            break;
        case LAUNCHER_TAB_TOOLS:
            LauncherTabPanelBegin("##tools", scale, pageOffset, pageAlpha);
            LauncherToolsGuiUpd();
            LauncherTabPanelEnd();
            break;
        case LAUNCHER_TAB_KENG:
            LauncherTabPanelBegin("##keng", scale, pageOffset, pageAlpha);
            LauncherKengGuiUpd();
            LauncherTabPanelEnd();
            break;
        case LAUNCHER_TAB_SETTINGS:
            LauncherTabPanelBegin("##settings", scale, pageOffset, pageAlpha);
            LauncherCfgGuiUpd();
            LauncherTabPanelEnd();
            break;
        case LAUNCHER_TAB_PLUGINS:
            LauncherTabPanelBegin("##plugins", scale, pageOffset, pageAlpha);
            DrawExtensionManager(locale, !pluginsWereOpen, RefreshPluginTools, DrawPluginSettings);
            LauncherTabPanelEnd();
            break;
        default:
            break;
        }
        pluginsWereOpen = activeTab == LAUNCHER_TAB_PLUGINS;
        if (activeTab >= 0 && activeTab != LAUNCHER_TAB_SETTINGS)
            LauncherCloseHotkeyRebindListeners();
        ImGui::EndChild();

        ImGui::EndChild();
        ImGui::End();
        LauncherWndEndFrame(wndPos, wndSize, canMove);

        switch (gLauncherTrigger) {
        case THPrac::LAUNCHER_RESTART:
            localeSwitch = Gui::LocaleGet();
            break;
        case THPrac::LAUNCHER_CLOSE:
        case THPrac::LAUNCHER_RESET:
            isOpen = false;
            break;
        case THPrac::LAUNCHER_MINIMIZE:
            LauncherWndMinimize();
            break;
        default:
            break;
        }
        if (gLauncherTrigger != LAUNCHER_RESET) {
            gLauncherTrigger = LAUNCHER_NOTHING;
        }

        if (localeSwitch != -1) {
            MSG msg;
            Gui::LocaleSet((Gui::locale_t)localeSwitch);
            LauncherWndShutdown();
            while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            }
            LauncherWndInit(640, 480, 1280, 960, 960, 720);
            localeSwitch = -1;
        }
    }
    LauncherGamesDestroy();
    LauncherOthersDestroy();
    LauncherKengDestroy();
    // DisableDisableWinKey();

    LauncherCfgClose();
    LauncherWndShutdown();
    if (gLauncherTrigger == LAUNCHER_RESET) {
        LauncherCfgReset();
    }
    
    return 0;
}

void GuiLauncherHotkeyInit()
{
    if (LauncherCfgInit(true)) {
        if (!Gui::MenuChordInitFromCfg()) {
            Gui::MenuChordAutoSet();
        }
        LauncherCfgClose();
    }

    Gui::MenuChordInitArrays();
}

void InitLocaleAndChore()
{
    if (!Gui::LocaleInitFromCfg()) {
        Gui::LocaleAutoSet();
    }
    // Load menu open key chords
    if (!Gui::MenuChordInitFromCfg()) {
        Gui::MenuChordAutoSet();
    }

    // Done after loading language as its string entries rely on it.
    Gui::MenuChordInitArrays();
}

}
