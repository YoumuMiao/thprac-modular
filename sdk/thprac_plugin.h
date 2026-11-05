#ifndef THPRAC_PLUGIN_H
#define THPRAC_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#define THPRAC_PLUGIN_ABI_V1 1u
#define THPRAC_PLUGIN_QUERY "THPracPluginQuery"
#define THPRAC_PLUGIN_CALL __cdecl

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ThpracPluginContextV1 {
    uint32_t size;
    const wchar_t* data_directory; /* UTF-16, valid for this call */
    const char* game_id;           /* UTF-8, null in launcher */
    void* launcher_window;         /* HWND, null in game */
} ThpracPluginContextV1;

typedef void (THPRAC_PLUGIN_CALL *ThpracPluginCallbackV1)(const ThpracPluginContextV1*);

typedef struct ThpracPluginV1 {
    uint32_t size;
    const char* id;                 /* Unique stable ASCII ID */
    const char* title_utf8;
    ThpracPluginCallbackV1 launcher_action;
    ThpracPluginCallbackV1 game_ready;
} ThpracPluginV1;

/* Return a descriptor with static lifetime, or null for an unsupported ABI. */
typedef const ThpracPluginV1* (THPRAC_PLUGIN_CALL *ThpracPluginQueryFn)(uint32_t abi);

#ifdef __cplusplus
}
#endif

#endif
