#pragma once
#include <imgui.h>

namespace THPrac {
namespace Gui {
    // Cover art for one game row.
    //
    // Resolution order:
    //   1. "<data dir>\covers\<game id>.(png|jpg|jpeg|bmp|gif)" -- %APPDATA%\thprac,
    //      the same folder as thpracP.json, so a user can override any cover;
    //   2. "<exe dir>\covers\<game id>.<ext>" -- user artwork next to the exe.
    //
    // The first hit is decoded once with GDI+ and uploaded as a D3D9 texture; the
    // file timestamp is remembered so replacing the image shows up without a
    // restart.
    //
    // Returns false when there is no usable image -- the caller then draws the
    // procedural cover instead. `pixelSize` is the decoded size, used to keep
    // the aspect ratio when the card has to crop; `matte`, when given, receives
    // the average colour of the artwork (IM_COL32), which the card uses to tint
    // the bands around a letterboxed cover.
    bool LauncherCoverGet(const char* gameId, void** texture, ImVec2& pixelSize, ImU32* matte = nullptr);

    // Absolute path of the folder that is searched for cover images.
    const wchar_t* LauncherCoverFolder();

    // Secondary folder next to the executable (portable artwork).
    const wchar_t* LauncherCoverLocalFolder();

    // Drops every cached texture. Must be called before the D3D device dies.
    void LauncherCoverShutdown();
}
}
