#pragma once

#include <stdint.h>
#include <stddef.h>

namespace THPrac::QuickPractice {
inline constexpr char pluginId[] = "rueee-c7.quick-practice";
inline constexpr char editorExport[] = "THPracPracticeEditor";
inline constexpr uint32_t protocol = 1;

enum class Status : long { Pending, Claimed, Committed, Applied, Entered, Rejected, Cancelled };

struct Request {
    uint32_t version = protocol;
    char game[8] {};
    int difficulty = 3;
    int shot = 0;
    int stage = 0;
    uint32_t bytes = 0;
    unsigned char parameters[512] {};
};

enum class Edit : uint32_t { Probe, Reset, Draw, Save, RestoreSettings, SnapshotSettings };
struct EditorContext {
    void* imgui;
    int locale;
    void* (__cdecl *alloc)(size_t, void*);
    void (__cdecl *free)(void*, void*);
    void* allocContext;
};
using Editor = int (__cdecl *)(const char* game, const EditorContext*, Edit, Request*);

struct EntryPoint {
    uint32_t address; // RVA relative to the game EXE
    uint32_t length;
    uint8_t expected[8];
};
enum HookStep : unsigned { Title, Confirm, Display, HookCount };

struct Service {
    uint32_t size;
    int (__cdecl *supports)(const char* game);
    void* (__cdecl *create)(void* process, const Request*);
    Status (__cdecl *status)(void* ticket);
    void (__cdecl *close)(void* ticket);
    const Request* (__cdecl *claim)(const char* game);
    int (__cdecl *active)();
    int (__cdecl *update)(Status status);
    const EntryPoint* (__cdecl *entry)(const Request*, unsigned step);
    int (__cdecl *ready)(const Request*, const void* context);
    void (__cdecl *enter)(const Request*, void* context, unsigned step);
    void (__cdecl *complete_display)(const Request*);
};
}
