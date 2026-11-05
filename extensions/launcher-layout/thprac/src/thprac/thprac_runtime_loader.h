#pragma once
#include <Windows.h>

namespace THPrac {
constexpr char runtimeEntryPoint[] = "THPracRuntimeStart";
constexpr DWORD runtimeStarted = 0;
constexpr DWORD runtimeUnsupportedGame = 1;

enum class RemoteRuntimeResult { Started, Rejected, Unknown };

RemoteRuntimeResult StartRemoteRuntime(HANDLE process, const wchar_t* path);
}
