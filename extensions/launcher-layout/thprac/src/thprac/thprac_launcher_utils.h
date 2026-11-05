#pragma once

#if _WIN32 || _WIN64
#if _WIN64
#define ENVIRONMENT64
#else
#define ENVIRONMENT32
#endif
#endif

// Check GCC
#if __GNUC__
#if __x86_64__ || __ppc64__
#define ENVIRONMENT64
#else
#define ENVIRONMENT32
#endif
#endif

#define NOMINMAX
#include <Windows.h>
#include <imgui.h>
#include <string>
#include <functional>
#include <vector>
#include <ctime>
#include <imgui.h>
#pragma warning(disable : 4091)
#include <Shlobj.h>
#pragma warning(default : 4091)
#include "thprac_launcher_wnd.h"

namespace THPrac {

#define JsonAddMember(json, key, value, alloc) json.AddMember(rapidjson::Value(key, alloc).Move(), rapidjson::Value(value).Move(), alloc);
#define JsonAddMemberA(json, key, value, alloc) json.AddMember(rapidjson::Value(key, alloc).Move(), rapidjson::Value(value, alloc).Move(), alloc);

class GuiThread {
public:
    GuiThread() = default;
    GuiThread(const GuiThread&) = delete;
    GuiThread& operator=(GuiThread&) = delete;
    GuiThread(GuiThread&&) = delete;
    GuiThread& operator=(GuiThread&&) = delete;
    GuiThread(LPTHREAD_START_ROUTINE threadFunc)
    {
        mThreadFunc = threadFunc;
    }

    DWORD GetExitCode()
    {
        if (mThreadHnd != INVALID_HANDLE_VALUE) {
            DWORD exitCode;
            GetExitCodeThread(mThreadHnd, &exitCode);
            return exitCode;
        }
        return 0;
    }
    bool Start(void* threadData = nullptr)
    {
        if (mThreadHnd == INVALID_HANDLE_VALUE) {
            mThreadHnd = CreateThread(nullptr, 0, mThreadFunc, threadData, 0, nullptr);
            return mThreadHnd != INVALID_HANDLE_VALUE;
        }
        return false;
    }
    bool Stop()
    {
        if (mThreadHnd != INVALID_HANDLE_VALUE) {
            if (IsActive()) {
#pragma warning(push)
#pragma warning(disable: 6258)
                TerminateThread(mThreadHnd, 0);
#pragma warning(pop)
            }
            CloseHandle(mThreadHnd);
            mThreadHnd = INVALID_HANDLE_VALUE;
        }
        return true;
    }
    bool Wait()
    {
        if (mThreadHnd != INVALID_HANDLE_VALUE) {
            WaitForSingleObject(mThreadHnd, INFINITE);
            return true;
        }
        return false;
    }
    bool IsActive()
    {
        return GetExitCode() == STILL_ACTIVE;
    }
    void SetThreadFunc(LPTHREAD_START_ROUTINE threadFunc)
    {
        mThreadFunc = threadFunc;
    }

private:
    HANDLE mThreadHnd = INVALID_HANDLE_VALUE;
    LPTHREAD_START_ROUTINE mThreadFunc = nullptr;
};

class GuiWaitingAnm {
public:
    GuiWaitingAnm() = default;

    std::string Get()
    {
        mCounter++;
        if (mCounter >= 240) {
            mCounter = 0;
        }
        switch (mCounter) {
        case 0:
            mAscii = ".";
            break;
        case 80:
            mAscii = "..";
            break;
        case 160:
            mAscii = "...";
            break;
        default:
            break;
        }
        return mAscii;
    }
    void Reset()
    {
        mCounter = 0;
        mAscii = ".";
    }

private:
    int mCounter = 0;
    std::string mAscii = ".";
};

void MovWndToTop(HWND m_hWnd);
void GuiColumnText(const char* text);

inline void GuiCenteredText(const char* text)
{
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(text, 0, false, 0.0f).x) / 2.0f);
    ImGui::TextWrapped("%s", text);
}

inline void GuiSetPosXText(const char* text, float offset = 0.0f)
{
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(text, 0, false, 0.0f).x) / 2.0f - offset);
}

inline void GuiSetPosYRel(float rel)
{
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() * rel);
}

int GuiCornerButton(
    const char* text,
    const char* text2 = nullptr,
    const ImVec2& offset = ImVec2(1.5f, 0.5f),
    bool useCurrentY = false
);

inline bool GuiButtonRelCentered(const char* buttonText, float posYRel, const ImVec2& sizeRel)
{
    auto wndSize = ImGui::GetWindowSize();
    ImGui::SetCursorPosX((wndSize.x - wndSize.x * sizeRel.x) / 2.0f);
    ImGui::SetCursorPosY(wndSize.y * posYRel);
    return ImGui::Button(buttonText, ImVec2(wndSize.x * sizeRel.x, wndSize.y * sizeRel.y));
}

inline bool GuiButtonTxtCentered(const char* buttonText, float posYRel)
{
    auto wndSize = ImGui::GetWindowSize();
    auto textSize = ImGui::CalcTextSize(buttonText);
    ImGui::SetCursorPosX((wndSize.x - textSize.x) / 2.0f);
    ImGui::SetCursorPosY(wndSize.y * posYRel);
    return ImGui::Button(buttonText);
}

inline bool GuiModal(const char* modalTitle, ImVec2 sizeRel = ImVec2(0.0f, 0.0f),bool* popen=nullptr,ImGuiWindowFlags flagex=0)
{
    const ImVec2 wndSize = LauncherWndGetSize();
    ImGui::SetNextWindowPos(ImVec2(wndSize.x * 0.5f, wndSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (sizeRel.x != 0.0f || sizeRel.y != 0.0f) {
        ImGui::SetNextWindowSize(sizeRel, ImGuiCond_Always);
    }
    // A modal must stay inside the window that hosts it. An auto-fitted dialog
    // whose content is wide (long wrapped text, plus the corner buttons that are
    // positioned relative to the window width) kept growing until it hit the
    // viewport clamp, so it ended up wider than the launcher window and its
    // 应用/取消 buttons were drawn outside the visible area - while the modal
    // still blocked every other input.
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(wndSize.x * 0.96f, wndSize.y * 0.96f));
    const bool open = ImGui::BeginPopupModal(modalTitle, popen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | flagex);
    if (open) {
        // Re-centre with this frame's size. The auto-fit width can change after
        // the initial position was latched, and a dialog that only grows to the
        // right would leave the window again.
        const ImVec2 size = ImGui::GetWindowSize();
        ImVec2 pos((wndSize.x - size.x) * 0.5f, (wndSize.y - size.y) * 0.5f);
        if (pos.x < 0.0f)
            pos.x = 0.0f;
        if (pos.y < 0.0f)
            pos.y = 0.0f;
        ImGui::SetWindowPos(pos);
    }
    return open;
}

inline bool GuiModalFullScreen(const char* modalTitle)
{
    auto wndSize = LauncherWndGetSize();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(wndSize, ImGuiCond_Appearing);
    return ImGui::BeginPopupModal(modalTitle, nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
}

inline bool GuiButtonModal(const char* buttonText, const char* modalTitle)
{
    if (ImGui::Button(buttonText)) {
        ImGui::OpenPopup(modalTitle);
        return true;
    }
    return false;
}

bool GuiButtonYesNo(
    const char* buttonText1 = "OK",
    const char* buttonText2 = "Cancel",
    float buttonSize = -1.0f
);
bool GuiButtonAndModalYesNo(
    const char* buttonText,
    const char* modalTitle,
    const char* modalText,
    float buttonSize = 6.0f,
    const char* buttonText1 = "OK",
    const char* buttonText2 = "Cancel"
);

}
