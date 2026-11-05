#ifndef THPRAC_EXTENSION_H
#define THPRAC_EXTENSION_H
#include <stdint.h>
#include <stddef.h>

#define THPRAC_EXTENSION_ABI 2u
#define THPRAC_EXTENSION_QUERY "THPracExtensionQuery"

#ifdef __cplusplus
extern "C" {
#endif

struct ThpracExtension;
struct ThpracGameHost;
struct ThpracPluginCallbacks;
struct ThpracPluginUi;
struct ThpracPluginGameEvent;

typedef struct ThpracLauncherHost {
    uint32_t size;
    int (__cdecl *apply_to_game)(void* process, uintptr_t image_base);
    void (__cdecl *draw_tools)(int (__cdecl *button)(const char* id, const char* label), void* owner);
    void (__cdecl *refresh_plugins)();
    int (__cdecl *plugin_settings)(const wchar_t* path, const struct ThpracPluginUi* ui);
    const wchar_t* install_directory; /* Host EXE directory, independent of the DLL location. */
} ThpracLauncherHost;

typedef struct ThpracGameFrame {
    uint32_t size;
    int game;
    uint32_t game_info_visible;
    uintptr_t image_base;
    void* window;
    void* drawing;
    int (__cdecl *read_game_memory)(uintptr_t address, void* buffer, uint32_t bytes);
    void (__cdecl *draw_text_box)(void* drawing, float x, float y, float width, const char* utf8);
} ThpracGameFrame;

typedef struct ThpracExtension {
    uint32_t size;
    const char* id;
    int (__cdecl *run_launcher)(const ThpracLauncherHost*);
    void (__cdecl *on_game_frame)(const ThpracGameFrame*);
    int (__cdecl *replace_game_init)(const char* game_id);
    const char* launcher_action_title;
    void (__cdecl *launcher_action)(void* owner);
    int (__cdecl *start_game_adapter)(const struct ThpracGameHost*, const char* game_id);
    const void* service;
    const struct ThpracPluginCallbacks* plugin;
} ThpracExtension;

typedef struct ThpracGameHost {
    uint32_t size;
    uint32_t exe_timestamp;
    uint32_t exe_text_size;
    const ThpracExtension* (__cdecl *find_extension)(const char* id);
    void (__cdecl *dispatch_frame)(const ThpracGameFrame* frame);
    void (__cdecl *dispatch_plugin_event)(const struct ThpracPluginGameEvent* event);
    const wchar_t* install_directory; /* Host EXE directory, independent of the DLL location. */
} ThpracGameHost;

typedef const ThpracExtension* (__cdecl *ThpracExtensionQueryFn)(uint32_t abi);

#ifdef __cplusplus
}
#endif
#endif
