#pragma once

#include "../quick-practice/service.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <atomic>
#include <string>

namespace THPrac::QuickPractice {
struct LauncherSources {
    const Service* service = nullptr;
    Editor editor = nullptr;
    explicit operator bool() const { return service && editor; }
};

LauncherSources GetLauncherSources(const char* game);
void RefreshLauncherSources();
void RestoreLauncherSelection(const Service&, Request&);
void SaveLauncherSelection(const Request&);
bool Apply(HANDLE process, uintptr_t base, const Service&, const Request&, void*& ticket);
bool WaitForEntry(HANDLE process, const Service&, void* ticket, const std::atomic_bool& abort);
bool LaunchThcrap(const std::wstring& directory, const std::wstring& configuration,
    const Service&, const Request&, const std::atomic_bool& abort);
}
