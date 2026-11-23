"""Read a game's music index and prepare private copies of changed tracks."""

from configparser import ConfigParser
from dataclasses import dataclass
from hashlib import sha256
import json
import math
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import uuid
import wave

from prepare_bgm import Track, read_tracks


GENERATION = re.compile(r"[0-9a-f]{32}")
PCM = {1: ("pcm_u8", "u8"), 2: ("pcm_s16le", "s16le"), 3: ("pcm_s24le", "s24le"),
       4: ("pcm_s32le", "s32le")}


def executable(configured, name):
    path = Path(configured) if configured else None
    if path and path.is_absolute() and path.is_file():
        return path
    bundled = Path(__file__).resolve().with_name(f"{name}.exe")
    if bundled.is_file():
        return bundled
    found = shutil.which(name)
    return Path(found).resolve() if found else None


def run_tool(args, cwd, timeout=180):
    result = subprocess.run([str(arg) for arg in args], cwd=cwd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode:
        details = result.stdout.decode("utf-8", errors="replace").strip()
        raise ValueError(details[-1400:] or f"{Path(args[0]).name} 退出码 {result.returncode}")
    return result.stdout


def track_titles(path):
    if not path or not path.is_file():
        return {}
    contents = path.read_bytes()
    try:
        text = contents.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = contents.decode("cp932", errors="replace")
    titles = {}
    name = None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("@"):
            entry = line[1:].replace("\\", "/").rsplit("/", 1)[-1].lower()
            match = re.fullmatch(r"([a-z0-9_-]+)(?:\.(?:wav|mid))?", entry)
            name = match[1] + ".wav" if match else None
        elif name and line and not line.startswith("#"):
            title = re.sub(r"^No\.\s*\d+\s*", "", line, flags=re.IGNORECASE).lstrip("♪").strip()
            if title:
                titles.setdefault(name, title)
                name = None
    return titles


def extract_titles(thdat, version, archive):
    with tempfile.TemporaryDirectory(prefix="thprac-bgm-titles-") as directory:
        temp = Path(directory)
        try:
            run_tool([thdat, f"-x{version}", archive, "musiccmt.txt"], temp)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            return {}
        return track_titles(temp / "musiccmt.txt")


def th06_loops(folder, tracks, thdat):
    def collect(root):
        loops = {}
        for name, track in tracks.items():
            file = root / Path(name).with_suffix(".pos")
            if file.is_file() and file.stat().st_size == 8:
                start, end = struct.unpack("<II", file.read_bytes())
                if start < end <= track.length // track.frame_bytes:
                    loops[name] = (start, end)
        return loops

    loops = collect(folder / "bgm")
    if thdat and len(loops) < len(tracks):
        for archive in folder.iterdir():
            if not archive.is_file() or not archive.name.lower().endswith("md.dat"):
                continue
            with tempfile.TemporaryDirectory(prefix="thprac-bgm-pos-") as directory:
                try:
                    run_tool([thdat, "-x6", archive, *(Path(name).with_suffix(".pos").name
                                                      for name in tracks if name not in loops)], directory)
                except (OSError, ValueError, subprocess.TimeoutExpired):
                    continue
                loops = collect(Path(directory)) | loops
            if len(loops) == len(tracks):
                break
    return loops


def game_data(folder, code, trial=False):
    if code == "th06":
        return next((file for file in folder.iterdir()
                     if file.is_file() and file.name.lower().endswith("md.dat")), None)
    suffix = "_tr.dat" if trial else ".dat"
    names = {"th095": ("th095", "th95"), "th09": ("th09", "th09e")}.get(code, (code,))
    return next((folder / (name + suffix) for name in names if (folder / (name + suffix)).is_file()), None)


def find_index(folder, code, fmt_name, configured_fmt, thdat):
    def existing(name):
        return next((path for root in (folder, folder / "data")
                     if (path := root / name).is_file()), None)

    fmt = Path(configured_fmt) if configured_fmt else existing(fmt_name)
    titles = track_titles(existing("musiccmt.txt"))
    main_archive = game_data(folder, code, fmt_name == "thbgm_tr.fmt")
    if fmt and fmt.is_file():
        if not titles and thdat and main_archive:
            titles = extract_titles(thdat, int(code[2:]), main_archive)
        return fmt.read_bytes(), titles
    if not thdat or not main_archive:
        raise ValueError(f"找不到 {fmt_name}。请手动选择曲目表，或设置 thdat.exe 以从主数据包提取。")
    with tempfile.TemporaryDirectory(prefix="thprac-bgm-index-") as directory:
        temp = Path(directory)
        run_tool([thdat, "-x" + str(int(code[2:])), main_archive, fmt_name, "musiccmt.txt"], temp)
        extracted = temp / fmt_name
        if not extracted.is_file():
            raise ValueError(f"thdat 没有提取出 {fmt_name}，请检查游戏版本或手动选择曲目表。")
        return extracted.read_bytes(), titles or track_titles(temp / "musiccmt.txt")


@dataclass
class Library:
    folder: Path
    tracks: dict
    titles: dict
    archive: Path | None
    identity: str
    loops: dict
    code: str

    @property
    def manager(self):
        return self.folder / "thprac-bgm" / "manager" / (self.archive.stem.lower() if self.archive else "wav")


def load_library(folder, code, configured_fmt="", thdat=None, archive_name=""):
    folder = Path(folder).resolve(strict=True)
    if code == "th06":
        tracks = {}
        for number in range(1, 18):
            name = f"th06_{number:02}.wav"
            source = folder / "bgm" / name
            if not source.is_file():
                continue
            with wave.open(str(source), "rb") as audio:
                rate, channels, width, frames = (audio.getframerate(), audio.getnchannels(),
                                                 audio.getsampwidth(), audio.getnframes())
            if frames and width in PCM:
                tracks[name] = Track(name, 0, 0, frames * channels * width, rate, channels, width)
        if not tracks:
            raise ValueError("找不到红魔乡的 WAV 曲目。请选择包含 bgm 文件夹的游戏目录。")
        titles = track_titles(folder / "musiccmt.txt") or track_titles(folder / "data" / "musiccmt.txt")
        if not titles and thdat:
            for archive in folder.iterdir():
                if archive.is_file() and archive.name.lower().endswith("md.dat"):
                    titles = extract_titles(thdat, 6, archive)
                    if titles:
                        break
        return Library(folder, tracks, titles, None, code, th06_loops(folder, tracks, thdat), code)

    names = ("thbgm.dat", "thbgm_tr.dat")
    archive = next((folder / name for name in names if (not archive_name or archive_name == name)
                    and (folder / name).is_file()), None)
    if not archive:
        raise ValueError("所选游戏目录没有 thbgm.dat 或 thbgm_tr.dat。")
    fmt_bytes, titles = find_index(folder, code, archive.with_suffix(".fmt").name, configured_fmt, thdat)
    stat = archive.stat()
    tracks = read_tracks(fmt_bytes, stat.st_size)
    if code != "th128" and "th128_08.wav" in tracks:
        # Later games reuse this archive filename for Player's Score.
        titles.setdefault("th128_08.wav", "プレイヤーズスコア")
    identity = f"{code}:{archive.name}:{stat.st_size}:{stat.st_mtime_ns}:{sha256(fmt_bytes).hexdigest()}"
    loops = {name: (track.intro // track.frame_bytes, track.length // track.frame_bytes)
             for name, track in tracks.items()}
    return Library(folder, tracks, titles, archive, identity, loops, code)


def read_profile(library):
    pointer = ConfigParser()
    pointer.read(library.manager / "active.ini", encoding="ascii")
    generation = pointer.get("BGM", "generation", fallback="")
    if not GENERATION.fullmatch(generation):
        return None, {}
    folder = library.manager / generation
    profile = json.loads((folder / "profile.json").read_text(encoding="utf-8"))
    if profile["identity"] != library.identity:
        return None, {}
    sources = profile["sources"]
    if not isinstance(sources, dict) or any(name not in library.tracks for name in sources):
        raise ValueError("已保存的替换曲目与当前游戏目录不匹配。")
    return folder, sources


def copy_bytes(source, write, length, repeat=False):
    while length:
        part = source.read(min(length, 262144))
        if not part:
            if not repeat:
                raise ValueError("音频数据提前结束。")
            source.seek(0)
            continue
        write(part)
        length -= len(part)


def decode(ffmpeg, source, track, output, ending=0):
    codec, container = PCM[track.sample_bytes]
    duration = track.length / (track.frame_bytes * track.rate)
    run_tool([ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
              "-protocol_whitelist", "file,pipe", "-i", source, "-map", "0:a:0",
              "-vn", "-sn", "-dn", "-ar", track.rate, "-ac", track.channels,
              "-t", f"{max(duration, ending) + 0.1:.6f}", "-c:a", codec, "-f", container, output],
             output.parent)
    size = output.stat().st_size
    if not size or size % track.frame_bytes:
        raise ValueError("解码器没有生成完整的 PCM 音频帧。")


def normalize_replacement(source):
    if isinstance(source, str):
        source = {"path": source}
    if "candidates" not in source:
        choice = {"path": source["path"]}
        if source.get("loop") is not None:
            choice["loop"] = source["loop"]
        return {"candidates": [choice], "selected": 0, "random": False, "once": False}
    choices = source["candidates"]
    selected = source.get("selected", 0)
    if not choices or len(choices) > 1000 or not 0 <= selected < len(choices) or any(not choice.get("path") for choice in choices):
        raise ValueError("请选择有效的候选曲目。")
    return {"candidates": choices, "selected": selected, "random": bool(source.get("random")),
            "once": bool(source.get("once", True))}


def playback_configuration(active, sources):
    if not active:
        return {}
    profile = json.loads((active / "profile.json").read_text(encoding="utf-8"))
    old_loops = profile.get("loops", {})
    policies = profile.get("policies", {})
    return {name: normalize_replacement(policies.get(name, {"path": path, "loop": old_loops.get(name)}))
            for name, path in sources.items()}


def single_end(library, name):
    track = library.tracks[name]
    if name not in library.loops:
        raise ValueError(f"{name} 缺少原版循环点，无法启用单次播放。")
    end = library.loops[name][1]
    # Reserve a silent interval for the native stream to keep reading after playback.
    reserve = min(end // 4, track.rate * (4 if library.code == "th06" else 1))
    if not reserve:
        raise ValueError(f"{name} 的音频槽过短。")
    return end - reserve


def prepare_loop_metadata(library, policies, staged, thdat):
    once = {name: single_end(library, name) for name, policy in policies.items() if policy["once"]}
    if not once:
        return False
    trial = library.archive and library.archive.stem.lower() == "thbgm_tr"
    archive = game_data(library.folder, library.code, trial)
    if not thdat or not archive:
        raise ValueError("单次播放需要 thdat 和与游戏匹配的主数据包。")
    version = int(library.code[2:])
    repack = executable(str(Path(thdat).with_name("bgm_repack.exe")), "bgm_repack")
    if not repack:
        raise ValueError("缺少单次播放所需的 bgm_repack.exe。")
    with tempfile.TemporaryDirectory(prefix="data-", dir=staged) as directory:
        data = Path(directory)
        if library.code == "th06":
            run_tool([thdat, f"-x{version}", archive, *(Path(name).with_suffix(".pos").name for name in once)], data)
            for name, start in once.items():
                pos = data / Path(name).with_suffix(".pos")
                if struct.unpack("<II", pos.read_bytes()) != library.loops[name]:
                    raise ValueError(f"{name} 的 POS 循环点与游戏主数据包不一致。")
                pos.write_bytes(struct.pack("<II", start, library.loops[name][1]))
        else:
            fmt = data / library.archive.with_suffix(".fmt").name
            run_tool([thdat, f"-x{version}", archive, fmt.name], data)
            content = bytearray(fmt.read_bytes())
            native = read_tracks(bytes(content), library.archive.stat().st_size)
            if native != library.tracks:
                raise ValueError("曲目表与游戏主数据包不一致，无法生成单次播放。")
            for index, name in enumerate(native):
                if name in once:
                    struct.pack_into("<I", content, index * 52 + 24, once[name] * native[name].frame_bytes)
            fmt.write_bytes(content)
        run_tool([repack, version, archive, staged / ("meta-" + archive.name), data], data)
    return True


def write_silence(write, length, sample_bytes):
    block = bytes([128 if sample_bytes == 1 else 0]) * 262144
    while length:
        chunk = min(length, len(block))
        write(block[:chunk])
        length -= chunk


def write_custom_loop(ffmpeg, decoded, track, custom, native, write):
    start, end = (round(seconds * track.rate) for seconds in custom)
    original_start, original_end = native
    if start >= end or end * track.frame_bytes > decoded.stat().st_size:
        raise ValueError("替换音频循环点无效，或循环终点超过音频时长。")
    codec, container = PCM[track.sample_bytes]
    fitted = decoded.with_name("section.pcm")
    for first, last, frames in ((0, start, original_start), (start, end, original_end - original_start)):
        if not frames:
            continue
        if last == first:
            write_silence(write, frames * track.frame_bytes, track.sample_bytes)
            continue
        if last - first == frames:
            with decoded.open("rb") as pcm:
                pcm.seek(first * track.frame_bytes)
                copy_bytes(pcm, write, frames * track.frame_bytes)
            continue
        tempo = (last - first) / frames
        filters = [f"atrim=start_sample={first}:end_sample={last}", "asetpts=PTS-STARTPTS"]
        while tempo > 2:
            filters.append("atempo=2")
            tempo /= 2
        while tempo < 0.5:
            filters.append("atempo=0.5")
            tempo *= 2
        filters.extend((f"atempo={tempo:.10f}", f"apad=whole_len={frames}", f"atrim=end_sample={frames}"))
        run_tool([ffmpeg, "-nostdin", "-loglevel", "error", "-y", "-f", container,
                  "-ar", track.rate, "-ac", track.channels, "-i", decoded, "-af", ",".join(filters),
                  "-c:a", codec, "-f", container, fitted], decoded.parent)
        with fitted.open("rb") as pcm:
            copy_bytes(pcm, write, frames * track.frame_bytes)
        fitted.unlink()
    # TH06 WAVs can extend past the native loop end.
    write_silence(write, track.length - original_end * track.frame_bytes, track.sample_bytes)


def fade_replacement(pcm, track, native, once_frames=None):
    """Soften initial playback and the edges of the game's loop."""
    def fade(first, count, incoming):
        if count < 2:
            return
        offset = first * track.frame_bytes
        pcm.seek(offset)
        data = bytearray(pcm.read(count * track.frame_bytes))
        width = track.sample_bytes
        signed = width != 1
        zero = 0 if signed else 128
        for frame in range(count):
            gain = (frame if incoming else count - frame - 1) / (count - 1)
            frame_start = frame * track.frame_bytes
            for pos in range(frame_start, frame_start + track.frame_bytes, width):
                sample = int.from_bytes(data[pos:pos + width], "little", signed=signed) - zero
                data[pos:pos + width] = (round(sample * gain) + zero).to_bytes(width, "little", signed=signed)
        pcm.seek(offset)
        pcm.write(data)

    frames = track.length // track.frame_bytes
    if once_frames is not None:
        fade(0, min(once_frames // 2, round(track.rate * 0.12)), True)
        tail = min(once_frames // 2, round(track.rate * 0.08))
        fade(once_frames - tail, tail, False)
    elif native:
        start, end = native
        edge = min(round(track.rate * 0.008), (end - start) // 2)
        if start:
            intro_edge = min(edge, start // 2)
            fade(0, min(start - intro_edge, round(track.rate * 0.12)), True)
            fade(start - intro_edge, intro_edge, False)
        fade(start, edge, True)
        fade(end - edge, edge, False)
    else:
        fade(0, min(frames, round(track.rate * 0.12)), True)


def render_candidate(ffmpeg, track, choice, native, once_end, staged, output):
    custom = choice.get("loop") if once_end is None else None
    if custom is not None and (not native or len(custom) != 2
            or not all(math.isfinite(value) for value in custom) or not 0 <= custom[0] < custom[1]):
        raise ValueError("请输入有效的替换音频循环起止时间。")
    decoded = staged / "audio.pcm"
    decode(ffmpeg, Path(choice["path"]), track, decoded, custom[1] if custom else 0)
    with decoded.open("rb") as pcm, output.open("w+b") as fitted:
        if once_end is not None:
            audible = min(decoded.stat().st_size, once_end * track.frame_bytes)
            copy_bytes(pcm, fitted.write, audible)
            write_silence(fitted.write, track.length - audible, track.sample_bytes)
            fade_replacement(fitted, track, None, audible // track.frame_bytes)
        else:
            if custom:
                write_custom_loop(ffmpeg, decoded, track, custom, native, fitted.write)
            else:
                copy_bytes(pcm, fitted.write, track.length, repeat=True)
            fade_replacement(fitted, track, native)
    decoded.unlink()


def write_wave(path, pcm_file, track):
    with wave.open(str(path), "wb") as audio, pcm_file.open("rb") as pcm:
        audio.setparams((track.channels, track.sample_bytes, track.rate, 0, "NONE", "not compressed"))
        copy_bytes(pcm, audio.writeframesraw, track.length)


def apply_changes(library, changes, ffmpeg, thdat=None):
    active, sources = read_profile(library)
    policies = playback_configuration(active, sources)
    if not changes or any(name not in library.tracks for name in changes):
        raise ValueError("请选择有效的替换曲目。")
    if any(changes.values()) and not ffmpeg:
        raise ValueError("找不到 ffmpeg.exe。请先在工具路径中选择它。")
    library.manager.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="staging-", dir=library.manager) as directory:
        staged = Path(directory)
        archive = library.archive
        if archive:
            target = staged / archive.name
            previous = active / archive.name if active and sources else archive
            shutil.copyfile(previous, target)
        for name, track in library.tracks.items():
            variants = staged / "variants" / Path(name).stem
            if name not in changes:
                if not archive and name in sources:
                    shutil.copyfile(active / name, staged / name)
                if active and (active / variants.relative_to(staged)).is_dir():
                    shutil.copytree(active / variants.relative_to(staged), variants)
                continue
            source = changes[name]
            if source:
                policy = normalize_replacement(source)
                once_end = single_end(library, name) if policy["once"] else None
                choices = policy["candidates"]
                selected = policy["selected"]
                indexes = list(range(len(choices))) if policy["random"] else [selected]
                for index in indexes:
                    prepared_pcm = staged / "prepared.pcm"
                    render_candidate(ffmpeg, track, choices[index], library.loops.get(name), once_end, staged, prepared_pcm)
                    if archive and index == selected:
                        with target.open("r+b") as package, prepared_pcm.open("rb") as fitted:
                            package.seek(track.offset)
                            copy_bytes(fitted, package.write, track.length)
                    if not archive and index == selected:
                        write_wave(staged / name, prepared_pcm, track)
                    if policy["random"] and index != selected:
                        variants.mkdir(parents=True, exist_ok=True)
                        if archive:
                            prepared_pcm.rename(variants / f"{index}.pcm")
                        else:
                            write_wave(variants / f"{index}.wav", prepared_pcm, track)
                    prepared_pcm.unlink(missing_ok=True)
                sources[name] = choices[selected]["path"]
                policies[name] = policy
            elif name in sources:
                if archive:
                    with archive.open("rb") as original, target.open("r+b") as package:
                        original.seek(track.offset)
                        package.seek(track.offset)
                        copy_bytes(original, package.write, track.length)
                del sources[name]
                policies.pop(name, None)
        if archive and not sources:
            target.unlink()
        if prepare_loop_metadata(library, policies, staged, thdat):
            (staged / "requires-metadata").touch()
        random_tracks = [(name, policy) for name, policy in policies.items()
                         if policy["random"] and len(policy["candidates"]) > 1]
        if archive and random_tracks:
            (staged / "variants.txt").write_text("".join(
                f"{library.tracks[name].offset} {library.tracks[name].length} "
                f"{len(policy['candidates'])} {policy['selected']} {Path(name).stem}\n"
                for name, policy in random_tracks), encoding="ascii")
        if not archive and random_tracks:
            (staged / "variants.ini").write_text("[WAV]\n" + "".join(
                f"{name}={len(policy['candidates'])}\n" for name, policy in random_tracks), encoding="ascii")
        (staged / "in-use").touch()

        generation = uuid.uuid4().hex
        (staged / "profile.json").write_text(json.dumps(
            {"identity": library.identity, "sources": sources, "policies": policies}, ensure_ascii=False), encoding="utf-8")
        prepared = library.manager / generation
        staged.rename(prepared)
        try:
            with tempfile.NamedTemporaryFile(dir=library.manager, delete=False) as pointer:
                pending = Path(pointer.name)
                pointer.write(f"[BGM]\ngeneration={generation}\n".encode("ascii"))
            try:
                pending.replace(library.manager / "active.ini")
            finally:
                pending.unlink(missing_ok=True)
        except OSError:
            shutil.rmtree(prepared, ignore_errors=True)
            raise

    for folder in library.manager.iterdir():
        if folder != prepared and GENERATION.fullmatch(folder.name) and not folder.is_symlink():
            try:
                (folder / "in-use").unlink(missing_ok=True)
            except OSError:
                continue
            shutil.rmtree(folder, ignore_errors=True)
    return sources
