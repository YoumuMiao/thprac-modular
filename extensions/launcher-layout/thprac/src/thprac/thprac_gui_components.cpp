#include "thprac_gui_components.h"
#include "imgui_internal.h"
#include "thprac_boss_capture.h"
#include <Shlwapi.h>
#include <cmath>
#include <format>

namespace THPrac
{
    void HelpMarker(const char* desc)
    {
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    void CustomMarker(const char* text, const char* desc)
    {
        ImGui::TextDisabled(text);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }


    int rotation_start_index;
    int scale_start_index;
    void ImRotateStart()
    {
        rotation_start_index = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    }
    
    ImVec2 ImRotationCenter()
    {
        ImVec2 l(FLT_MAX, FLT_MAX), u(-FLT_MAX, -FLT_MAX); // bounds
    
        const auto& buf = ImGui::GetWindowDrawList()->VtxBuffer;
        for (int i = rotation_start_index; i < buf.Size; i++)
            l = ImMin(l, buf[i].pos), u = ImMax(u, buf[i].pos);
    
        return ImVec2((l.x + u.x) / 2, (l.y + u.y) / 2); // or use _ClipRectStack?
    }
    
    ImVec2 operator-(const ImVec2& l, const ImVec2& r) { return { l.x - r.x, l.y - r.y }; }
    
    void ImRotateEnd(float rad, ImVec2 center)
    {
        float s = sin(rad), c = cos(rad);
        center = ImRotate(center, s, c) - center;
    
        auto& buf = ImGui::GetWindowDrawList()->VtxBuffer;
        for (int i = rotation_start_index; i < buf.Size; i++)
            buf[i].pos = ImRotate(buf[i].pos, s, c) - center;
    }

    void ImScaleStart()
    {
        scale_start_index = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    }

    ImVec2 ImScaleCenter()
    {
        ImVec2 l(FLT_MAX, FLT_MAX), u(-FLT_MAX, -FLT_MAX); // bounds
        const auto& buf = ImGui::GetWindowDrawList()->VtxBuffer;
        for (int i = scale_start_index; i < buf.Size; i++)
            l = ImMin(l, buf[i].pos), u = ImMax(u, buf[i].pos);

        return ImVec2((l.x + u.x) / 2, (l.y + u.y) / 2); // or use _ClipRectStack?
    }


    void ImScaleEnd(float scaleX, float scaleY, ImVec2 center)
    {
        center = ImVec2 (center.x * scaleX, center.y * scaleY) - center;

        auto& buf = ImGui::GetWindowDrawList()->VtxBuffer;
        for (int i = scale_start_index; i < buf.Size; i++)
            buf[i].pos = ImVec2(buf[i].pos.x * scaleX, buf[i].pos.y * scaleY) - center;
    }

	namespace Gui
	{
        int LauncherActionGrid(const LauncherAction* actions, int count)
        {
            const float font = ImGui::GetFontSize();
            const float gap = font * 1.2f;
            const float available = ImGui::GetContentRegionAvail().x;
            float longest = 0.0f;
            for (int i = 0; i < count; ++i)
                longest = ImMax(longest, ImGui::CalcTextSize(actions[i].label).x);
            const int columns = available >= 2.0f * (longest + font * 2.0f) + gap ? 2 : 1;
            int clicked = -1;
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(gap * 0.5f, font * 0.4f));
            if (ImGui::BeginTable("##tool_actions", columns,
                    ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX)) {
                for (int i = 0; i < count; ++i) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(i);
                    const float width = ImGui::GetContentRegionAvail().x;
                    const float wrap = ImMax(1.0f, width - font * 1.5f);
                    const ImVec2 textSize = ImGui::CalcTextSize(actions[i].label, nullptr, false, wrap);
                    const float height = ImMax(font * 2.6f, textSize.y + font);
                    if (ImGui::Button("##action", ImVec2(width, height))) clicked = i;
                    const ImVec2 min = ImGui::GetItemRectMin();
                    const ImVec2 max = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->PushClipRect(min, max, true);
                    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), font,
                        ImVec2(min.x + (width - textSize.x) * 0.5f, min.y + (height - textSize.y) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_Text), actions[i].label, nullptr, wrap);
                    ImGui::GetWindowDrawList()->PopClipRect();
                    if (actions[i].help && ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(font * 24.0f);
                        ImGui::TextUnformatted(actions[i].help);
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
            return clicked;
        }

        int FeaturePageSelector(int current, bool practiceControls)
        {
            const char* pages[] = {
                DesignText("练习辅助", "Practice", "練習補助"),
                DesignText("信息显示", "Display", "情報表示"),
                DesignText("声音", "Audio", "サウンド"),
                DesignText("统计记录", "Statistics", "統計記録")
            };
            if (ImGui::BeginTabBar("##feature_pages")) {
                for (int i = practiceControls ? 0 : 1; i < 4; ++i) {
                    if (ImGui::BeginTabItem(pages[i])) {
                        current = i;
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
            return current;
        }

        void PracticeHeader()
        {
            const auto& palette = GetChromeColors();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette.sectionTitle), "%s", S(TH_MENU));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", DesignText("↑↓ 选择项目 · ←→ 调整数值", "Up/Down: select · Left/Right: adjust", "上下: 項目選択 · 左右: 値の変更"));
            ImGui::PopStyleColor();
        }

        void PracticeSection(bool resources)
        {
            const auto& palette = GetChromeColors();
            const char* text = resources
                ? DesignText("初始资源", "Starting resources", "開始時のリソース")
                : DesignText("练习目标", "Practice target", "練習対象");
            ImGui::Spacing();
            const auto pos = ImGui::GetCursorScreenPos();
            const float font = ImGui::GetFontSize();
            ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + 3.0f, pos.y + font), palette.sectionRail, 1.5f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 9.0f);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette.sectionTitle), "%s", text);
        }

		bool GuiNavFocus::mGlobalDisable = false;
        static std::vector<GameGuiWnd*> mouseOnlyWindows;
        void GameGuiWnd::SetMouseOnly()
        {
            if (!mMouseOnly) mouseOnlyWindows.push_back(this);
            mMouseOnly = true;
            mWndFlag |= ImGuiWindowFlags_NoNav;
        }
        bool GameGuiWnd::MouseOnlyWindowOpen()
        {
            for (auto* window : mouseOnlyWindows)
                if (!window->IsClosed()) return true;
            return false;
        }
        void GameGuiWnd::Update()
            {
                OnPreUpdate();

                if (mLocale != LocaleGet()) {
                    mLocale = LocaleGet();
                    OnLocaleChange();
                }

                ImGuiIO& io = ImGui::GetIO();
                size_t styleCount = 0;

                if (mStatus == 1) {
                    mAlpha += mAlphaStep;
                    if (mAlpha >= mAlphaMax) {
                        mAlpha = mAlphaMax;
                        mStatus = 2;
                    }
                } else if (mStatus == 3) {
                    io.MouseDrawCursor = false;
                    mAlpha -= mAlphaStep;
                    if (mAlpha <= 0.0f) {
                        mAlpha = 0.0f;
                        mStatus = 0;
                    }
                }

                if (!MouseOnlyWindowOpen()) {
                io.NavInputs[ImGuiNavInput_DpadUp] = InGameInputGet(VK_UP);
                io.NavInputs[ImGuiNavInput_DpadDown] = InGameInputGet(VK_DOWN);
                io.NavInputs[ImGuiNavInput_DpadLeft] = InGameInputGet(VK_LEFT);
                io.NavInputs[ImGuiNavInput_DpadRight] = InGameInputGet(VK_RIGHT);
                }

                if (mStatus) {
                    for (auto& style : mStyle) {
                        ImGui::PushStyleVarAlt(style.style, style.value);
                        styleCount++;
                    }
                    if (mPracticePanel) {
                        const float font = ImGui::GetFontSize();
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, font * 0.55f);
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(font * 0.7f, font * 0.55f));
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, font * 0.22f);
                        styleCount += 4;
                        ImGui::PushStyleColor(ImGuiCol_WindowBg, GetChromeColors().cardBackground);
                        ImGui::PushStyleColor(ImGuiCol_Border, GetChromeColors().cardBorder);
                        ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImGui::ColorConvertU32ToFloat4(GetChromeColors().sectionRail));
                    }
                    if (mInformationPanel) {
                        const float font = ImGui::GetFontSize();
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ImMax(6.0f, font * 0.6f));
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(font * 0.55f, font * 0.4f));
                        styleCount += 3;
                        const bool light = IsLightPalette();
                        ImGui::PushStyleColor(ImGuiCol_WindowBg, light ? GetChromeColors().cardBackground : ImVec4(8.0f/255,12.0f/255,10.0f/255,1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Border, light ? GetChromeColors().cardBorder : ImVec4(28.0f/255,157.0f/255,91.0f/255,0.74f));
                        ImGui::PushStyleColor(ImGuiCol_Separator, light ? ImGui::GetStyleColorVec4(ImGuiCol_Separator) : ImVec4(0.11f,0.35f,0.24f,0.55f));
                        ImGui::PushStyleColor(ImGuiCol_Text, light ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImVec4(236.0f/255,244.0f/255,240.0f/255,1.0f));
                    }
                    if (mFullScreen) {
                        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
                        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
                    } else {
                        if (mSizeFlag) {
                            ImGui::SetNextWindowSize(mSize, ImGuiCond_Always); // ImGuiCond_FirstUseEver
                            mSizeFlag = false;
                        }
                        if (mPosFlag) {

                            ImGui::SetNextWindowPos(mPos, ImGuiCond_Always);
                            mPosFlag = false;
                        }
                    }
                    if (mFocus)
                        ImGui::SetNextWindowFocus();

                    if (mInformationPanel) {
                        const ImVec2 size = mInformationPanelLastSize;
                        const ImVec2 pos(ImClamp(mPos.x,4.0f,ImMax(4.0f,io.DisplaySize.x-size.x-4.0f)),
                            ImClamp(mPos.y,4.0f,ImMax(4.0f,io.DisplaySize.y-size.y-4.0f)));
                        ImGui::SetNextWindowPos(pos,ImGuiCond_Always);
                    }
                    ImGui::SetNextWindowBgAlpha(mInformationPanel ? mAlpha * (IsLightPalette() ? 0.96f : 0.74f) : mAlpha);
                    ImGui::Begin(mTitle.c_str(), nullptr, mWndFlag);
                    ImGui::PushItemWidth(mItemWidth);

                    if (mStatus == 2) {
                        OnContentUpdate();
                        if (mInformationPanel && mInlineCapture) {
                            ImGui::Columns(1);
                            BossCapture::DrawInfoPanelCapture();
                        }
                    }

                    ImGui::PopItemWidth();
                    if (mInformationPanel) mInformationPanelLastSize = ImGui::GetWindowSize();
                    ImGui::End();
                    if (mPracticePanel) ImGui::PopStyleColor(3);
                    if (mInformationPanel) ImGui::PopStyleColor(4);
                    for (size_t i = 0; i < styleCount; i++)
                        ImGui::PopStyleVar();
                }

                OnPostUpdate();
            }
        void GameGuiWnd::Open()
        {
            if (mStatus != 2)
                mStatus = 1;
        }
        void GameGuiWnd::Close()
        {
            if (mStatus)
                mStatus = 3;
        }

        void GameGuiWnd::CloseImmediately()
        {
            mStatus = 0;
            mAlpha = 0.0f;
        }

        bool GameGuiWnd::IsOpen()
        {
            return mStatus == 2;
        }
        bool GameGuiWnd::IsClosed()
        {
            return mStatus == 0;
        }

        void GameGuiWnd::SetViewport(void* ptr)
        {
            mViewport = (Viewport*)ptr;
        }
        void GameGuiWnd::SetSize(float width, float height)
        {
            mSizeFlag = true;
            mSize.x = width;
            mSize.y = height;
        }
        void GameGuiWnd::SetPos(float x, float y)
        {
            mPosFlag = true;
            mPos.x = x;
            mPos.y = y;
        }
        void GameGuiWnd::SetFullScreen(bool toggle)
        {
            mFullScreen = toggle;
            mSizeFlag = true;
            mPosFlag = true;
        }
        void GameGuiWnd::SetItemWidth(float item_width)
        {
            mItemWidth = item_width;
        }
        void GameGuiWnd::SetSizeRel(float width_prop, float height_prop)
        {
            ImGuiIO& io = ImGui::GetIO();
            mSizeFlag = true;
            if (mViewport) {
                mSize.x = width_prop * (float)mViewport->Width;
                mSize.y = height_prop * (float)mViewport->Height;
            } else {
                mSize.x = width_prop * io.DisplaySize.x;
                mSize.y = height_prop * io.DisplaySize.y;
            }
        }
        void GameGuiWnd::SetPosRel(float x_prop, float y_prop)
        {
            ImGuiIO& io = ImGui::GetIO();
            mPosFlag = true;
            if (mViewport) {
                mPos.x = x_prop * (float)mViewport->Width + (float)mViewport->X;
                mPos.y = y_prop * (float)mViewport->Height + (float)mViewport->Y;
            } else {
                mPos.x = x_prop * io.DisplaySize.x;
                mPos.y = y_prop * io.DisplaySize.y;
            }
        }
        void GameGuiWnd::SetItemWidthRel(float item_width_prop)
        {
            ImGuiIO& io = ImGui::GetIO();
            if (mViewport) {
                mItemWidth = item_width_prop * (float)mViewport->Width;
            } else {
                mItemWidth = item_width_prop * io.DisplaySize.x;
            }
        }
        void GameGuiWnd::SetItemSpacing(float x, float y)
        {
            auto itemSpacing = GetStyleIt(ImGuiStyleVar_ItemSpacing);
            itemSpacing->value = ImVec2(x, y);
        }
        void GameGuiWnd::SetItemSpacingRel(float x_prop, float y_prop)
        {
            ImGuiIO& io = ImGui::GetIO();
            auto itemSpacing = GetStyleIt(ImGuiStyleVar_ItemSpacing);
            if (mViewport) {
                itemSpacing->value = ImVec2(x_prop * (float)mViewport->Width, y_prop * (float)mViewport->Height);
            } else {
                itemSpacing->value = ImVec2(x_prop * io.DisplaySize.x, y_prop * io.DisplaySize.y);
            }
        }
        void GameGuiWnd::SetAutoSpacing(bool toggle)
        {
            if (toggle) {
                ImGuiIO& io = ImGui::GetIO();
                if (mViewport) {
                    auto framePadding = GetStyleIt(ImGuiStyleVar_FramePadding);
                    framePadding->value = ImVec2(
                        0.00625f * (float)mViewport->Width, 0.00625f * (float)mViewport->Height);
                    auto itemSpacing = GetStyleIt(ImGuiStyleVar_ItemSpacing);
                    itemSpacing->value = ImVec2(
                        0.0125f * (float)mViewport->Width, 0.00833f * (float)mViewport->Height);
                } else {
                    auto framePadding = GetStyleIt(ImGuiStyleVar_FramePadding);
                    framePadding->value = ImVec2(
                        0.00625f * io.DisplaySize.x, 0.00625f * io.DisplaySize.y);
                    auto itemSpacing = GetStyleIt(ImGuiStyleVar_ItemSpacing);
                    itemSpacing->value = ImVec2(
                        0.0125f * io.DisplaySize.x, 0.00833f * io.DisplaySize.y);
                }
            } else {
                auto framePadding = GetStyleIt(ImGuiStyleVar_FramePadding);
                mStyle.erase(framePadding);
                auto itemSpacing = GetStyleIt(ImGuiStyleVar_ItemSpacing);
                mStyle.erase(itemSpacing);
            }
        }
        void GameGuiWnd::AutoSize(ImVec2 size,
            const char* content_1, const char* content_2, const char* label_1, const char* label_2,
            float widiget_counts, ImVec2 size_max, ImVec2 size_min)
        {
            auto& io = ImGui::GetIO();
            auto& style = ImGui::GetStyle();

            ImVec2 label_size = { -1.0f, -1.0f };
            if (label_1) {
                auto label_size_1 = ImGui::CalcTextSize(label_1);
                if (label_2) {
                    auto label_size_2 = ImGui::CalcTextSize(label_2);
                    label_size = label_size_1.x > label_size_2.x ? label_size_1 : label_size_2;
                } else
                    label_size = label_size_1;
            }
            bool label_valid = label_size.x >= 0.0f && label_size.y >= 0.0f;

            ImVec2 content_size = { -1.0f, -1.0f };
            if (content_1) {
                auto content_size_1 = ImGui::CalcTextSize(content_1);
                if (content_2) {
                    auto content_size_2 = ImGui::CalcTextSize(content_2);
                    content_size = content_size_1.x > content_size_2.x ? content_size_1 : content_size_2;
                } else
                    content_size = content_size_1;
            }
            bool content_valid = content_size.x >= 0.0f && content_size.y >= 0.0f;

            if (size.x >= 1.0f) {
                mSize.x = size.x;
            } else if (size.x > 0.0f) {
                mSize.x = io.DisplaySize.x * size.x;
            } else if (size.x == 0.0f && content_valid && label_valid) {
                mSize.x = content_size.x + label_size.x + ImGui::GetFrameHeight() * 3;
            }
            if (size_max.x >= 0.0f && mSize.x > size_max.x)
                mSize.x = size_max.x;
            else if (size_min.x >= 0.0f && mSize.x < size_min.x)
                mSize.x = size_min.x;

            if (size.y > 1.0f) {
                mSize.y = size.y;
            } else if (size.y > 0.0f) {
                mSize.y = io.DisplaySize.y * size.y;
            } else if (size.y == 0.0f) {
                float font_y = 0.0f;
                if (label_valid)
                    font_y = label_size.y;
                else
                    font_y = ImGui::GetFont()->FontSize;

                mSize.y = (font_y + style.FramePadding.y * 2.0f + style.ItemSpacing.y) * widiget_counts;
            }
            if (size_max.y >= 0.0f && mSize.y > size_max.y)
                mSize.y = size_max.y;
            else if (size_min.y >= 0.0f && mSize.y < size_min.y)
                mSize.y = size_min.y;

            if (label_valid)
                mItemWidth = label_size.x * -1.0f;

            mSizeFlag = true;
        }
        void GameGuiWnd::AutoSize(float x, float y, const char* content, const char* label, float widigit_counts, float max_y)
        {
            auto& io = ImGui::GetIO();
            auto& style = ImGui::GetStyle();

            if (x >= 1.0001f) {
                mSize.x = x;
            } else if (x > 0.0f) {
                mSize.x = io.DisplaySize.x * x;
            } else if (x == 0.0f && content && label) {
                mSize.x = ImGui::CalcTextSize(content).x + ImGui::CalcTextSize(label).x + ImGui::GetFrameHeight() * 3;
            }

            if (y > 1.0001f) {
                mSize.y = y;
            } else if (y > 0.0f) {
                mSize.y = io.DisplaySize.y * y;
            } else if (y == 0.0f) {
                float font_y = 0.0f;
                if (content || label)
                    font_y = ImGui::CalcTextSize(content ? content : label).y;
                else
                    font_y = ImGui::GetFont()->FontSize;

                mSize.y = (font_y + style.FramePadding.y * 2.0f + style.ItemSpacing.y) * widigit_counts;
                if (mSize.y > max_y)
                    mSize.y = max_y;
            }

            if (label)
                mItemWidth = ImGui::CalcTextSize(label).x * -1.0f;

            mSizeFlag = true;
        }
        void GameGuiWnd::AutoPos(float x, float y)
        {
            auto& io = ImGui::GetIO();

            if (x > 1.0001f) {
                mPos.x = x;
            } else if (x >= 0.0f) {
                mPos.x = (io.DisplaySize.x - mSize.x) * x;
            }

            if (y > 1.0001f) {
                mPos.y = y;
            } else if (y >= 0.0f) {
                mPos.y = (io.DisplaySize.y - mSize.y) * y;
            }

            mPosFlag = true;
        }
        void GameGuiWnd::AutoItemWidth(const char* label)
        {
            mItemWidth = ImGui::CalcTextSize(label).x * -1.0f;
        }

        void GameGuiWnd::SetStyle(ImGuiStyleVar style, float param1, float param2)
        {
            auto s = GetStyleIt(style);
            s->value = ImVec2(param1, param2);
        }
        void GameGuiWnd::SetWndFlag(ImGuiWindowFlags flags)
        {
            mWndFlag = flags | (mMouseOnly ? ImGuiWindowFlags_NoNav : 0);
        }
        void GameGuiWnd::SetFade(float transparency, float step)
        {
            mAlphaMax = transparency;
            mAlphaStep = step;
        }
        void GameGuiWnd::SetTitle(const char* title)
        {
            mTitle = title;
        }
        void GameGuiWnd::SetWndFoucs(bool toggle)
        {
            mFocus = toggle;
        }

        void GameGuiWnd::WndDebugOutput()
        {
            SetWndFlag(ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
            auto pos = ImGui::GetWindowPos();
            auto size = ImGui::GetWindowSize();
            auto& io = ImGui::GetIO();

            ImGui::Text("Size: %f (%f), %f (%f)", size.x, size.x / io.DisplaySize.x, size.y, size.y / io.DisplaySize.y);
            ImGui::Text("Pos X: %f (%f)", pos.x, pos.x / (io.DisplaySize.x - size.x));
            ImGui::Text("Pos Y: %f (%f)", pos.y, pos.y / (io.DisplaySize.y - size.y));
        }
        std::vector<GameGuiWnd::StyleValue>::iterator GameGuiWnd::GetStyleIt(ImGuiStyleVar style)
        {
            for (auto it = mStyle.begin(); it != mStyle.end(); ++it) {
                if (it->style == style) {
                    return it;
                }
            }
            mStyle.emplace_back();
            auto it = std::prev(mStyle.end());
            it->style = style;
            return it;
        }

        bool GuiCombo::operator()(int skip)
        {
            auto items = LocaleGetCurrentGlossary();
            CheckComboItemNew(mSelector, items, 1);

            const char* label;
            if (mLabelRef != A0000ERROR_C)
                label = LocaleGetStr(mLabelRef);
            else if (mLabel)
                label = mLabel;
            else
                label = "#ERROR";

            const char* _skip = skip == -1 ? "" : LocaleGetStr(mSelector[skip]);
            auto hasChanged = ImGui::ComboSections(label, &mCurrent, mSelector, items, _skip);

            if ((!GameGuiWnd::MouseOnlyWindowOpen() && ImGui::IsItemFocused())) {
                if (InGameInputGet(VK_LEFT)) {
                    hasChanged = true;
                    --mCurrent;
                    if (skip != -1 && mCurrent == skip)
                        --mCurrent;
                    if (mCurrent < 0) {
                        for (mCurrent = 0; mSelector[mCurrent]; ++mCurrent)
                            ;
                        mCurrent--;
                    }
                    CheckComboItemNew(mSelector, items, -1);
                } else if (InGameInputGet(VK_RIGHT)) {
                    hasChanged = true;
                    ++mCurrent;
                    if (skip != -1 && mCurrent == skip)
                        ++mCurrent;
                    if (mSelector[mCurrent] == 0) {
                        mCurrent = 0;
                    }
                    CheckComboItemNew(mSelector, items, 1);
                }
            }

            return hasChanged;
        }

        bool GuiButton::operator()()
        {
            const char* label = mLabel ? mLabel : LocaleGetStr(mLabelRef);
            bool res = ImGui::Button(label, mSize);
            if ((!GameGuiWnd::MouseOnlyWindowOpen() && ImGui::IsItemFocused()) && InGameInputGetConfirm())
                res = true;
            return res;
        }

        bool GuiCheckBox::operator()()
        {
            const char* label = mLabel ? mLabel : LocaleGetStr(mLabelRef);
            bool pressed = ImGui::Checkbox(label, &mToggle);
            if ((!GameGuiWnd::MouseOnlyWindowOpen() && ImGui::IsItemFocused()) && (InGameInputGet(VK_LEFT) || InGameInputGet(VK_RIGHT))) {
                mToggle = !mToggle;
                pressed = true;
            }
            return pressed;
        }



        bool GuiHotKey::OnWidgetUpdate()
        {
            const char* text = mText ? mText : LocaleGetStr(mTextRef);
            std::string realText;
            if (mStatus) {
                realText = std::format("[{}: {}]", mKeyText, text);
                ImGui::PushStyleColor(ImGuiCol_Text, { 0.0f, 1.0f, 0.0f, 1.0f });
            } else {
                realText = std::format("{}: {}", mKeyText, text);
            }

            auto cursor = ImGui::GetCursorPos();
            ImGui::TextUnformatted(realText.c_str());
            ImGui::SetCursorPos(cursor);

            if (mStatus)
                ImGui::PopStyleColor();
                        
            if (ImGui::InvisibleButton(mKeyText, ImGui::CalcTextSize(realText.c_str())))
                return true;
            else
                return false;
        }
        bool GuiHotKey::operator()(bool use_widget)
        {
            bool flag = Gui::KeyboardInputUpdate(mKey) == 1;
            if (use_widget)
                flag |= OnWidgetUpdate();

            if (flag) {
                mStatus = !mStatus;
                if (mStatus) {
                    for (size_t i = 0; i < mHooks.len; i++) {
                        mHooks.ptr[i].Enable();
                    }
                } else {
                    for (size_t i = 0; i < mHooks.len; i++) {
                        mHooks.ptr[i].Disable();
                    }
                }
            }       

            return flag;
        }

         // Almost the same as normal but checks for chord pressed instead of the key itself.
        bool GuiHotKeyChord::operator()(bool use_widget)
        {
            bool flag = Gui::GetChordPressed(mKey);
            if (use_widget)
                flag |= OnWidgetUpdate();

            if (flag) {
                mStatus = !mStatus;
                if (mStatus) {
                    for (size_t i = 0; i < mHooks.len; i++) {
                        mHooks.ptr[i].Enable();
                    }
                } else {
                    for (size_t i = 0; i < mHooks.len; i++) {
                        mHooks.ptr[i].Disable();
                    }
                }
            }

            return flag;
        }

        // void GuiTimer::Start()
        // {
        //     if (!mIsTiming) {
        //         QueryPerformanceCounter(&mStart);
        //         mIsTiming = true;
        //     }
        // }
        // void GuiTimer::End()
        // {
        //     if (mIsTiming) {
        //         LARGE_INTEGER end;
        //         QueryPerformanceCounter(&end);
        //         mElapsed.QuadPart = end.QuadPart - mStart.QuadPart;
        //         mIsTiming = false;
        //     }
        // }
        // long long GuiTimer::GetElapsedUs()
        // {
        //     return (mElapsed.QuadPart * 1000000 / mFreq.QuadPart);
        // }

        void GuiNavFocus::operator()()
        {
            if (mGlobalDisable)
                return;

            bool has_active_popup = false;
            int last_activated_id = 0;

            for (auto id : mNavId) {
                // TODO: Does `id` really need to be an int?
                auto idAsGlossaryRef = static_cast<th_glossary_t>(id);
                if (ImGui::IsPopupOpen(LocaleGetStr(idAsGlossaryRef)))
                    has_active_popup = true;

                if (ImGui::IsItemActiveAlt(LocaleGetStr(idAsGlossaryRef)))
                    last_activated_id = id;
                else if (ImGui::IsItemFocusedAlt(LocaleGetStr(idAsGlossaryRef)) && !mForceFocusId)
                    mFocusId = id;
            }

            if (!has_active_popup)
                ImGui::SetWindowFocus();

            if (last_activated_id && !mForceFocusId)
                mFocusId = last_activated_id;

            bool locale_changed = false;
            if (mLocale != LocaleGet()) {
                mLocale = LocaleGet();
                locale_changed = true;
            }

            // TODO: Do `mForceFocusId`, `mFocusId`, and `mNavId` need to be ints?
            if (mForceFocusId)
                ImGui::SetItemFocusAlt(LocaleGetStr(static_cast<th_glossary_t>(mForceFocusId)), true);
            else if (mFocusId)
                ImGui::SetItemFocusAlt(LocaleGetStr(static_cast<th_glossary_t>(mFocusId)), locale_changed);
            else
                ImGui::SetItemFocusAlt(LocaleGetStr(static_cast<th_glossary_t>(mNavId[0])), locale_changed);

            if (mForceFocusId) {
                mFocusId = mForceFocusId;
                mForceFocusId = 0;
            }
        }

        void ComboSelect(size_t& out, const char* const* choices, const size_t choices_count, const char* label)
        {
            if (ImGui::BeginCombo(label, choices[out])) {
                for (size_t i = 0; i < choices_count; i++) {
                    ImGui::PushID(i);

                    bool item_selected = (i == out);

                    if (ImGui::Selectable(choices[i], &item_selected))
                        out = i;

                    if (item_selected)
                        ImGui::SetItemDefaultFocus();

                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }

            if ((!GameGuiWnd::MouseOnlyWindowOpen() && ImGui::IsItemFocused())) {
                if (Gui::InGameInputGet(VK_LEFT) && out > 0) {
                    out--;
                }
                if (Gui::InGameInputGet(VK_RIGHT) && out + 1 < choices_count) {
                    out++;
                }
            }
        }

        void ComboSelect(size_t& out, th_glossary_t* choices, const size_t choices_count, const char* label)
        {
            if (ImGui::BeginCombo(label, S(choices[out]))) {
                for (size_t i = 0; i < choices_count; i++) {
                    ImGui::PushID(i);

                    bool item_selected = (i == out);

                    if (ImGui::Selectable(S(choices[i]), &item_selected))
                        out = i;

                    if (item_selected)
                        ImGui::SetItemDefaultFocus();

                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }

            if ((!GameGuiWnd::MouseOnlyWindowOpen() && ImGui::IsItemFocused())) {
                if (Gui::InGameInputGet(VK_LEFT) && out > 0) {
                    out--;
                }
                if (Gui::InGameInputGet(VK_RIGHT) && out + 1 < choices_count) {
                    out++;
                }
            }
        }

        void MultiComboSelectImpl(std::vector<size_t>& out, const char* const* choices, const size_t choices_count, const char* format, size_t level)
        {
            if (out.size() <= level)
                out.resize(level + 1);

            size_t bufSize = snprintf(nullptr, 0, format, level);
            auto labelStr = new char[bufSize + 2];
            labelStr[bufSize + 1] = 0;
            snprintf(labelStr, bufSize + 1, format, level);

            ComboSelect(out[level], choices, choices_count, labelStr);

            if (out[level]) {
                ImGui::PushID(++level);
                MultiComboSelectImpl(out, choices, choices_count, format, level);
                ImGui::PopID();
            }

            delete[] labelStr;
        }

        void MultiComboSelect(std::vector<size_t>& out, const char* const* choices, const size_t choices_count, const char* format)
        {
            MultiComboSelectImpl(out, choices, choices_count, format, 0);
        }

        // Hue of the icon accent in degrees: #1C9D5B is a spring green, 149.3 deg.
        static constexpr float ICON_GREEN_HUE = 149.30f;
        // Entries below this saturation are neutral greys (borders, dimming
        // layers); recoloring them would tint the whole UI, so they are skipped.
        static constexpr float ACCENT_MIN_SATURATION = 0.13f;
        // Hue window covering cyan, blue and violet accents of the stock themes.
        static constexpr float ACCENT_HUE_BEGIN = 165.0f;
        static constexpr float ACCENT_HUE_END = 285.0f;

        static void RgbToHsv(float r, float g, float b, float& h, float& s, float& v)
        {
            const float maxValue = (r > g ? (r > b ? r : b) : (g > b ? g : b));
            const float minValue = (r < g ? (r < b ? r : b) : (g < b ? g : b));
            const float delta = maxValue - minValue;
            v = maxValue;
            s = (maxValue > 0.0f) ? delta / maxValue : 0.0f;
            if (delta <= 0.0f) {
                h = 0.0f;
                return;
            }
            if (maxValue == r) {
                h = 60.0f * std::fmod((g - b) / delta, 6.0f);
                if (h < 0.0f)
                    h += 360.0f;
            } else if (maxValue == g) {
                h = 60.0f * ((b - r) / delta + 2.0f);
            } else {
                h = 60.0f * ((r - g) / delta + 4.0f);
            }
        }

        static void HsvToRgb(float h, float s, float v, float& r, float& g, float& b)
        {
            h = std::fmod(h, 360.0f);
            if (h < 0.0f)
                h += 360.0f;
            const float chroma = v * s;
            const float second = chroma * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
            const float base = v - chroma;
            float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
            if (h < 60.0f) {
                r1 = chroma;
                g1 = second;
            } else if (h < 120.0f) {
                r1 = second;
                g1 = chroma;
            } else if (h < 180.0f) {
                g1 = chroma;
                b1 = second;
            } else if (h < 240.0f) {
                g1 = second;
                b1 = chroma;
            } else if (h < 300.0f) {
                r1 = second;
                b1 = chroma;
            } else {
                r1 = chroma;
                b1 = second;
            }
            r = r1 + base;
            g = g1 + base;
            b = b1 + base;
        }

        void ApplyIconGreenAccent()
        {
            // Rec.709 weights: the recolor keeps the perceived lightness of the
            // original entry so widget contrast stays exactly as before.
            constexpr float LUMA_R = 0.2126f;
            constexpr float LUMA_G = 0.7152f;
            constexpr float LUMA_B = 0.0722f;

            ImVec4* colors = ImGui::GetStyle().Colors;
            for (int i = 0; i < ImGuiCol_COUNT; i++) {
                ImVec4& color = colors[i];
                float h, s, v;
                RgbToHsv(color.x, color.y, color.z, h, s, v);
                if (s < ACCENT_MIN_SATURATION || v <= 0.0f)
                    continue;
                if (h < ACCENT_HUE_BEGIN || h > ACCENT_HUE_END)
                    continue;

                // Brightness of the accent hue at full value, used to normalize.
                float rUnit, gUnit, bUnit;
                HsvToRgb(ICON_GREEN_HUE, s, 1.0f, rUnit, gUnit, bUnit);
                const float unitLuma = LUMA_R * rUnit + LUMA_G * gUnit + LUMA_B * bUnit;
                if (unitLuma <= 0.0f)
                    continue;

                const float originalLuma = LUMA_R * color.x + LUMA_G * color.y + LUMA_B * color.z;
                float targetValue = originalLuma / unitLuma;
                if (targetValue > 1.0f)
                    targetValue = 1.0f;
                HsvToRgb(ICON_GREEN_HUE, s, targetValue, color.x, color.y, color.z);
            }
        }

        static ImVec4 JadeTint(const ImVec4& color, float alpha)
        {
            return ImVec4(color.x, color.y, color.z, alpha);
        }

        // Geometry shared by both jade variants. Everything is expressed in
        // units of a 20px base font so the recipe reads correctly at 640x480
        // (16px) and at DPI-scaled launcher sizes.
        static void ApplyJadeMetrics()
        {
            ImGuiStyle& style = ImGui::GetStyle();
            const float fontSize = ImGui::GetFontSize();
            const float u = fontSize > 0.0f ? fontSize / 20.0f : 1.0f;

            style.WindowPadding = ImVec2(16.0f * u, 14.0f * u);
            style.WindowRounding = 0.0f;
            style.WindowBorderSize = 0.0f;
            style.WindowMinSize = ImVec2(32.0f * u, 32.0f * u);
            style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
            style.ChildRounding = 8.0f * u;
            style.ChildBorderSize = 1.0f;
            style.PopupRounding = 8.0f * u;
            style.PopupBorderSize = 1.0f;
            style.FramePadding = ImVec2(9.0f * u, 5.0f * u);
            style.FrameRounding = 6.0f * u;
            style.FrameBorderSize = 1.0f;
            style.ItemSpacing = ImVec2(9.0f * u, 7.0f * u);
            style.ItemInnerSpacing = ImVec2(7.0f * u, 5.0f * u);
            style.CellPadding = ImVec2(6.0f * u, 4.0f * u);
            style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
            style.IndentSpacing = 20.0f * u;
            style.ColumnsMinSpacing = 6.0f * u;
            style.ScrollbarSize = 12.0f * u;
            style.ScrollbarRounding = 6.0f * u;
            style.GrabMinSize = 11.0f * u;
            style.GrabRounding = 5.0f * u;
            style.TabRounding = 8.0f * u;
            style.TabBorderSize = 0.0f;
            style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
            style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
            style.DisplaySafeAreaPadding = ImVec2(3.0f, 3.0f);
        }

        void StyleColorsJade()
        {
            ImGui::StyleColorsDark();
            ApplyJadeMetrics();

            // Surfaces: near-black green, tinted towards the accent so panels
            // read as one material instead of stock grey.
            const ImVec4 bgDeep { 0.0431f, 0.0784f, 0.0667f, 1.00f }; // #0B1411
            const ImVec4 bgPanel { 0.0588f, 0.1137f, 0.0902f, 1.00f }; // #0F1D17
            const ImVec4 bgRaised { 0.0706f, 0.1412f, 0.1098f, 1.00f }; // #12241C
            const ImVec4 accent { ICON_GREEN_R, ICON_GREEN_G, ICON_GREEN_B, 1.00f };
            const ImVec4 accentBright { 0.2078f, 0.8510f, 0.5373f, 1.00f }; // #35D989
            const ImVec4 accentSoft { 0.2314f, 0.8784f, 0.5490f, 1.00f }; // #3BE08C
            const ImVec4 textMain { 0.9098f, 0.9529f, 0.9255f, 1.00f }; // #E8F3EC
            const ImVec4 textFaint { 0.56f, 0.65f, 0.60f, 1.00f };

            ImVec4* colors = ImGui::GetStyle().Colors;
            colors[ImGuiCol_Text] = textMain;
            colors[ImGuiCol_TextDisabled] = textFaint;
            colors[ImGuiCol_WindowBg] = bgDeep;
            colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
            colors[ImGuiCol_PopupBg] = bgRaised;
            colors[ImGuiCol_Border] = JadeTint(accent, 0.28f);
            colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
            colors[ImGuiCol_FrameBg] = JadeTint(accent, 0.13f);
            colors[ImGuiCol_FrameBgHovered] = JadeTint(accentBright, 0.24f);
            colors[ImGuiCol_FrameBgActive] = JadeTint(accentBright, 0.34f);
            colors[ImGuiCol_TitleBg] = bgPanel;
            colors[ImGuiCol_TitleBgActive] = JadeTint(accent, 0.55f);
            colors[ImGuiCol_TitleBgCollapsed] = bgPanel;
            colors[ImGuiCol_MenuBarBg] = bgPanel;
            colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.16f);
            colors[ImGuiCol_ScrollbarGrab] = JadeTint(accent, 0.55f);
            colors[ImGuiCol_ScrollbarGrabHovered] = JadeTint(accentBright, 0.75f);
            colors[ImGuiCol_ScrollbarGrabActive] = accentBright;
            colors[ImGuiCol_CheckMark] = accentSoft;
            colors[ImGuiCol_SliderGrab] = accent;
            colors[ImGuiCol_SliderGrabActive] = accentSoft;
            colors[ImGuiCol_Button] = JadeTint(accent, 0.22f);
            colors[ImGuiCol_ButtonHovered] = JadeTint(accentBright, 0.26f);
            colors[ImGuiCol_ButtonActive] = JadeTint(accent, 0.90f);
            colors[ImGuiCol_Header] = JadeTint(accent, 0.20f);
            colors[ImGuiCol_HeaderHovered] = JadeTint(accentBright, 0.30f);
            colors[ImGuiCol_HeaderActive] = JadeTint(accent, 0.55f);
            colors[ImGuiCol_Separator] = JadeTint(accent, 0.20f);
            colors[ImGuiCol_SeparatorHovered] = JadeTint(accentBright, 0.55f);
            colors[ImGuiCol_SeparatorActive] = accentBright;
            colors[ImGuiCol_ResizeGrip] = JadeTint(accentBright, 0.18f);
            colors[ImGuiCol_ResizeGripHovered] = JadeTint(accentBright, 0.50f);
            colors[ImGuiCol_ResizeGripActive] = accentBright;
            colors[ImGuiCol_Tab] = JadeTint(accent, 0.10f);
            colors[ImGuiCol_TabHovered] = JadeTint(accentBright, 0.34f);
            colors[ImGuiCol_TabActive] = accent;
            colors[ImGuiCol_TabUnfocused] = JadeTint(accent, 0.06f);
            colors[ImGuiCol_TabUnfocusedActive] = JadeTint(accent, 0.42f);
            colors[ImGuiCol_PlotLines] = accentBright;
            colors[ImGuiCol_PlotLinesHovered] = accentSoft;
            colors[ImGuiCol_PlotHistogram] = accent;
            colors[ImGuiCol_PlotHistogramHovered] = accentSoft;
            colors[ImGuiCol_TableHeaderBg] = bgRaised;
            colors[ImGuiCol_TableBorderStrong] = JadeTint(accent, 0.35f);
            colors[ImGuiCol_TableBorderLight] = JadeTint(accent, 0.16f);
            colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
            colors[ImGuiCol_TableRowBgAlt] = JadeTint(accent, 0.04f);
            colors[ImGuiCol_TextSelectedBg] = JadeTint(accentBright, 0.35f);
            colors[ImGuiCol_DragDropTarget] = accentSoft;
            colors[ImGuiCol_NavHighlight] = accentSoft;
            colors[ImGuiCol_NavWindowingHighlight] = JadeTint(accentSoft, 0.70f);
            colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.02f, 0.05f, 0.04f, 0.55f);
            colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.05f, 0.04f, 0.70f);
        }

        void StyleColorsLightJade()
        {
            ImGui::StyleColorsLight();
            ApplyJadeMetrics();

            const ImVec4 bgPage { 0.9490f, 0.9686f, 0.9529f, 1.00f }; // #F2F7F3
            const ImVec4 bgRaised { 1.0000f, 1.0000f, 1.0000f, 1.00f }; // #FFFFFF
            const ImVec4 bgPanel { 0.8980f, 0.9412f, 0.9176f, 1.00f }; // #E5F0EA
            const ImVec4 accent { ICON_GREEN_R, ICON_GREEN_G, ICON_GREEN_B, 1.00f };
            const ImVec4 accentDeep { 0.0745f, 0.4706f, 0.2745f, 1.00f }; // #137845
            const ImVec4 accentMid { 0.2863f, 0.7686f, 0.5412f, 1.00f }; // #49C48A
            const ImVec4 textMain { 0.1020f, 0.1804f, 0.1412f, 1.00f }; // #1A2E24
            const ImVec4 textFaint { 0.36f, 0.45f, 0.40f, 1.00f };

            ImVec4* colors = ImGui::GetStyle().Colors;
            colors[ImGuiCol_Text] = textMain;
            colors[ImGuiCol_TextDisabled] = textFaint;
            colors[ImGuiCol_WindowBg] = bgPage;
            colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
            colors[ImGuiCol_PopupBg] = bgRaised;
            colors[ImGuiCol_Border] = JadeTint(accent, 0.32f);
            colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
            colors[ImGuiCol_FrameBg] = JadeTint(accent, 0.08f);
            colors[ImGuiCol_FrameBgHovered] = JadeTint(accent, 0.16f);
            colors[ImGuiCol_FrameBgActive] = JadeTint(accent, 0.24f);
            colors[ImGuiCol_TitleBg] = bgPanel;
            colors[ImGuiCol_TitleBgActive] = JadeTint(accent, 0.42f);
            colors[ImGuiCol_TitleBgCollapsed] = bgPanel;
            colors[ImGuiCol_MenuBarBg] = bgPanel;
            colors[ImGuiCol_ScrollbarBg] = JadeTint(accent, 0.05f);
            colors[ImGuiCol_ScrollbarGrab] = JadeTint(accent, 0.40f);
            colors[ImGuiCol_ScrollbarGrabHovered] = JadeTint(accent, 0.60f);
            colors[ImGuiCol_ScrollbarGrabActive] = accent;
            colors[ImGuiCol_CheckMark] = accentDeep;
            colors[ImGuiCol_SliderGrab] = accent;
            colors[ImGuiCol_SliderGrabActive] = accentDeep;
            colors[ImGuiCol_Button] = JadeTint(accent, 0.14f);
            colors[ImGuiCol_ButtonHovered] = JadeTint(accent, 0.18f);
            colors[ImGuiCol_ButtonActive] = JadeTint(accent, 0.44f);
            colors[ImGuiCol_Header] = JadeTint(accent, 0.12f);
            colors[ImGuiCol_HeaderHovered] = JadeTint(accent, 0.20f);
            colors[ImGuiCol_HeaderActive] = JadeTint(accent, 0.30f);
            colors[ImGuiCol_Separator] = JadeTint(accent, 0.22f);
            colors[ImGuiCol_SeparatorHovered] = JadeTint(accent, 0.45f);
            colors[ImGuiCol_SeparatorActive] = accent;
            colors[ImGuiCol_ResizeGrip] = JadeTint(accent, 0.16f);
            colors[ImGuiCol_ResizeGripHovered] = JadeTint(accent, 0.38f);
            colors[ImGuiCol_ResizeGripActive] = accent;
            colors[ImGuiCol_Tab] = JadeTint(accent, 0.07f);
            colors[ImGuiCol_TabHovered] = JadeTint(accent, 0.16f);
            colors[ImGuiCol_TabActive] = accentMid;
            colors[ImGuiCol_TabUnfocused] = JadeTint(accent, 0.05f);
            colors[ImGuiCol_TabUnfocusedActive] = JadeTint(accent, 0.30f);
            colors[ImGuiCol_PlotLines] = accentDeep;
            colors[ImGuiCol_PlotLinesHovered] = accent;
            colors[ImGuiCol_PlotHistogram] = accent;
            colors[ImGuiCol_PlotHistogramHovered] = accentDeep;
            colors[ImGuiCol_TableHeaderBg] = bgPanel;
            colors[ImGuiCol_TableBorderStrong] = JadeTint(accent, 0.38f);
            colors[ImGuiCol_TableBorderLight] = JadeTint(accent, 0.16f);
            colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
            colors[ImGuiCol_TableRowBgAlt] = JadeTint(accent, 0.04f);
            colors[ImGuiCol_TextSelectedBg] = JadeTint(accent, 0.28f);
            colors[ImGuiCol_DragDropTarget] = accentDeep;
            colors[ImGuiCol_NavHighlight] = accentDeep;
            colors[ImGuiCol_NavWindowingHighlight] = JadeTint(accentDeep, 0.70f);
            colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.85f, 0.90f, 0.87f, 0.55f);
            colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.09f, 0.16f, 0.12f, 0.45f);
        }

        void StyleColorsDarkGreen()
        {
            StyleColorsJade();
        }

        bool IsLightPalette()
        {
            const ImVec4& bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            return (0.2126f * bg.x + 0.7152f * bg.y + 0.0722f * bg.z) > 0.5f;
        }

        static const ChromeColors CHROME_DARK = {
            /* headerLeft            */ IM_COL32(20, 70, 48, 255),
            /* headerRight           */ IM_COL32(16, 55, 38, 255),
            /* headerTopHighlight    */ IM_COL32(150, 240, 190, 46),
            /* headerRule            */ IM_COL32(59, 224, 140, 95),
            /* title                 */ IM_COL32(238, 249, 243, 255),
            /* subtitle              */ IM_COL32(122, 158, 139, 255),
            /* version               */ IM_COL32(102, 136, 119, 255),
            /* glyph                 */ IM_COL32(198, 222, 208, 215),
            /* glyphHoverBackground  */ IM_COL32(255, 255, 255, 34),
            /* closeHoverBackground  */ IM_COL32(214, 72, 77, 235),
            /* pageTopLeft           */ IM_COL32(13, 31, 24, 255),
            /* pageTopRight          */ IM_COL32(11, 26, 20, 255),
            /* pageBottomRight       */ IM_COL32(6, 14, 11, 255),
            /* pageBottomLeft        */ IM_COL32(8, 18, 14, 255),
            /* pageGlow              */ IM_COL32(28, 157, 91, 30),
            /* pageFrame             */ IM_COL32(28, 157, 91, 70),
            /* cardBackground        */ ImVec4(0.062f, 0.120f, 0.094f, 1.00f),
            /* cardBorder            */ ImVec4(ICON_GREEN_R, ICON_GREEN_G, ICON_GREEN_B, 0.30f),
            /* sectionTitle          */ IM_COL32(140, 217, 179, 255),
            /* sectionRail           */ IM_COL32(59, 224, 140, 255),
            /* sectionLine           */ IM_COL32(59, 224, 140, 70),
            /* rowSubtitle           */ IM_COL32(124, 154, 138, 255),
            /* rowRail               */ IM_COL32(59, 224, 140, 235),
            /* rowRailHovered        */ IM_COL32(59, 224, 140, 110),
            /* dotAvailable          */ IM_COL32(59, 224, 140, 235),
            /* dotUnavailable        */ IM_COL32(120, 140, 130, 140),
            /* pointerGlow           */ IM_COL32(210, 255, 232, 255),
        };

        static const ChromeColors CHROME_LIGHT = {
            /* headerLeft            */ IM_COL32(233, 246, 238, 255),
            /* headerRight           */ IM_COL32(206, 232, 216, 255),
            /* headerTopHighlight    */ IM_COL32(255, 255, 255, 160),
            /* headerRule            */ IM_COL32(28, 157, 91, 80),
            /* title                 */ IM_COL32(20, 48, 35, 255),
            /* subtitle              */ IM_COL32(94, 124, 108, 255),
            /* version               */ IM_COL32(112, 138, 122, 255),
            /* glyph                 */ IM_COL32(58, 86, 72, 220),
            /* glyphHoverBackground  */ IM_COL32(28, 157, 91, 34),
            /* closeHoverBackground  */ IM_COL32(214, 72, 77, 235),
            /* pageTopLeft           */ IM_COL32(242, 248, 244, 255),
            /* pageTopRight          */ IM_COL32(238, 245, 241, 255),
            /* pageBottomRight       */ IM_COL32(227, 237, 231, 255),
            /* pageBottomLeft        */ IM_COL32(232, 241, 235, 255),
            /* pageGlow              */ IM_COL32(28, 157, 91, 26),
            /* pageFrame             */ IM_COL32(28, 157, 91, 60),
            /* cardBackground        */ ImVec4(1.000f, 1.000f, 1.000f, 1.00f),
            /* cardBorder            */ ImVec4(ICON_GREEN_R, ICON_GREEN_G, ICON_GREEN_B, 0.26f),
            /* sectionTitle          */ IM_COL32(24, 122, 73, 255),
            /* sectionRail           */ IM_COL32(28, 157, 91, 255),
            /* sectionLine           */ IM_COL32(28, 157, 91, 80),
            /* rowSubtitle           */ IM_COL32(88, 111, 99, 255),
            /* rowRail               */ IM_COL32(28, 157, 91, 235),
            /* rowRailHovered        */ IM_COL32(28, 157, 91, 120),
            /* dotAvailable          */ IM_COL32(28, 157, 91, 235),
            /* dotUnavailable        */ IM_COL32(150, 168, 158, 160),
            /* pointerGlow           */ IM_COL32(15, 107, 62, 255),
        };

        const ChromeColors& GetChromeColors()
        {
            return IsLightPalette() ? CHROME_LIGHT : CHROME_DARK;
        }
    }
}
