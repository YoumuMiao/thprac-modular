"""Background operations requested by the THPrac launcher music panel."""

import json
import os
from pathlib import Path
import sys
import tempfile

from bgm_library import apply_changes, executable, load_library, normalize_replacement, playback_configuration, read_profile


def save(path, value):
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
        pending = Path(output.name)
        output.write(json.dumps(value, ensure_ascii=False).encode("utf-8"))
    try:
        pending.replace(path)
    finally:
        pending.unlink(missing_ok=True)


def manage_presets(library, request, ffmpeg, thdat, current):
    path = library.manager / "presets.json"
    presets = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
    presets = {name: {track: normalize_replacement(policy) for track, policy in preset.items()}
               for name, preset in presets.items()}
    mode = request["mode"]
    if not mode.startswith("preset_"):
        return presets
    name = request.get("preset", "").strip()
    if not name or len(name) > 80 or any(ord(char) < 32 for char in name):
        raise ValueError("请输入 1–80 个字符的有效预设名。")
    if mode == "preset_save":
        if name in presets:
            raise ValueError("已有同名预设。请换一个名称，或选中原预设并更新。")
        presets[name] = current
    else:
        if name not in presets:
            raise ValueError("找不到此预设，请重新读取曲目。")
        if mode == "preset_apply":
            desired = presets[name]
            changes = {track: desired.get(track) for track in current.keys() | desired.keys()
                       if current.get(track) != desired.get(track)}
            if changes:
                apply_changes(library, changes, ffmpeg, thdat)
            return presets
        if mode == "preset_update":
            presets[name] = current
        elif mode == "preset_rename":
            new_name = request.get("preset_new_name", "").strip()
            if not new_name or len(new_name) > 80 or any(ord(char) < 32 for char in new_name):
                raise ValueError("请输入 1–80 个字符的有效新名称。")
            if new_name != name:
                if new_name in presets:
                    raise ValueError("已有同名预设，请换一个名称。")
                presets[new_name] = presets.pop(name)
        elif mode == "preset_delete":
            del presets[name]
    path.parent.mkdir(parents=True, exist_ok=True)
    save(path, presets)
    return presets


def run(request):
    settings_path = Path(os.environ["APPDATA"]) / "thprac" / "bgm-editor.json"
    try:
        settings = json.loads(settings_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        settings = {}
    games = settings.setdefault("games", {})
    mode = request["mode"]
    code = request.get("game") or settings.get("last", "th06")
    saved = games.get(code, {})
    if mode == "settings":
        return {"game": code, **saved,
                "ffmpeg": str(executable(settings.get("ffmpeg"), "ffmpeg") or ""),
                "thdat": str(executable(settings.get("thdat"), "thdat") or "")}

    settings.update({key: request[key] for key in ("ffmpeg", "thdat")})
    ffmpeg = executable(settings["ffmpeg"], "ffmpeg")
    thdat = executable(settings["thdat"], "thdat")
    library = load_library(request["folder"], code, request["fmt"], thdat, request["archive"])
    active, sources = read_profile(library)
    current = playback_configuration(active, sources)
    presets = manage_presets(library, request, ffmpeg, thdat, current)
    if mode == "apply":
        apply_changes(library, request["changes"], ffmpeg, thdat)
    if mode in ("apply", "preset_apply"):
        active, sources = read_profile(library)
        current = playback_configuration(active, sources)
    settings["last"] = code
    games[code] = {"folder": str(library.folder), "fmt": request["fmt"], "archive": request["archive"]}
    try:
        settings_path.parent.mkdir(parents=True, exist_ok=True)
        save(settings_path, settings)
    except OSError:
        pass
    legacy = library.folder / "thprac-bgm"
    legacy_archive = library.archive and (legacy / library.archive.name).is_file()
    return {"presets": [{"name": name, "matches": preset == current}
                        for name, preset in sorted(presets.items(), key=lambda item: item[0].casefold())],
            "tracks": [{"name": name, "title": library.titles.get(name, ""),
                        "source": sources.get(name, ""),
                        "policy": current.get(name),
                        "native_loop": library.loops.get(name),
                        "rate": track.rate,
                        "legacy": bool((legacy_archive and not sources)
                                       or (not library.archive and (legacy / name).is_file()))}
                       for name, track in library.tracks.items()]}


if __name__ == "__main__":
    request_path = Path(sys.argv[1])
    try:
        result = run(json.loads(request_path.read_text(encoding="utf-8")))
    except Exception as error:
        result = {"error": str(error)}
    save(request_path, result)
