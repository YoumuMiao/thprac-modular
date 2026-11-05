#pragma once

#include "thprac_extension.h"

struct ThpracVolumeService {
    uint32_t size;
    void (__cdecl *begin)();
    int (__cdecl *poll)(int* bgm, int* effects);
};

struct ThpracSettingsService {
    uint32_t size;
    // Called inside a game GUI frame. The window belongs to the game adapter.
    void (__cdecl *draw)(const struct ThpracSettingsUi*);
};

struct ThpracSettingsUi {
    uint32_t size;
    int locale;
    unsigned sections;
    void (__cdecl *heading)(const char*);
    int (__cdecl *tab)(const char*, int selected);
    void (__cdecl *next_tab)();
    void (__cdecl *section)(unsigned);
};

enum ThpracSettingsSection : unsigned {
    SettingsStatus = 1u,
    SettingsVolume = 2u,
    SettingsCapture = 4u
};

struct ThpracFeatureService {
    uint32_t size;
    int (__cdecl *supports_game)(const char*);
};

struct ThpracHudService {
    uint32_t size;
    unsigned (__cdecl *fields)();
    void (__cdecl *set_fields)(unsigned);
};
