#pragma once

#include <Windows.h>
#include <stdint.h>

namespace THPrac {
struct THGameSig;
uintptr_t GetGameModuleBase(HANDLE process);
THGameSig* CheckOngoingGameByPID(DWORD pid, uintptr_t* base, HANDLE* outHandle = nullptr);
bool ApplyGameRuntime(HANDLE process, uintptr_t base);
}
