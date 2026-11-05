#ifndef THPRAC_PLUGIN_SDK_H
#define THPRAC_PLUGIN_SDK_H
#include "thprac_extension.h"

#define THPRAC_PLUGIN_SDK_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ThpracPluginConfig {
    const char* plugin_id;
    int (__cdecl *get_int)(const char* plugin_id, const char* key, int fallback);
    int (__cdecl *set_int)(const char* plugin_id, const char* key, int value);
} ThpracPluginConfig;

typedef struct ThpracPluginUi {
    uint32_t size;
    int locale; /* 0 Chinese, 1 English, 2 Japanese */
    void (__cdecl *text)(const char* utf8);
    int (__cdecl *checkbox)(const char* utf8, int* enabled);
    ThpracPluginConfig config;
} ThpracPluginUi;

enum ThpracPluginEventKind {
    THPRAC_EVENT_GAME_READY = 1,
    THPRAC_EVENT_FRAME,
    THPRAC_EVENT_WINDOW_CLOSED,
    THPRAC_EVENT_STAGE_STARTED
};

typedef struct ThpracPluginOverlay {
    void* drawing;
    void (__cdecl *draw_text_box)(void* drawing, float x, float y, float width, const char* utf8);
} ThpracPluginOverlay;

typedef struct ThpracPluginStage {
    int stage_index; /* zero-based native stage index, -1 if unavailable */
    int shot;        /* native shot identifier, -1 if unavailable */
    int practice;    /* 1 in thprac practice, 0 otherwise, -1 if unknown */
    int replay;    /* 1 in replay, 0 otherwise, -1 if unknown */
} ThpracPluginStage;

typedef struct ThpracPluginGameEvent {
    uint32_t size;
    uint32_t kind;
    const char* game_id;
    uint32_t exe_timestamp;
    uint32_t exe_text_size;
    void* window;
    const ThpracPluginOverlay* overlay; /* FRAME only */
    const ThpracPluginStage* stage;    /* STAGE_STARTED only */
    ThpracPluginConfig config;
    /* GAME_READY only. Original receives the callable original import. */
    int (__cdecl *hook_import)(const char* dll, const char* symbol, void* replacement, void** original);
} ThpracPluginGameEvent;

typedef struct ThpracPluginCallbacks {
    uint32_t size;
    uint32_t version;
    const char* const* games; /* null-terminated game IDs, null means any supported game */
    void (__cdecl *draw_settings)(const ThpracPluginUi*);
    void (__cdecl *game_event)(const ThpracPluginGameEvent*);
} ThpracPluginCallbacks;

#ifdef __cplusplus
}
#endif
#endif
