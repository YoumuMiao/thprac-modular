"""Prepare separate BGM files for the THPrac C7 BGM plugin."""

import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile
from typing import NamedTuple
import wave


TRACK_RECORD = struct.Struct("<16sIIIIHHIIHH4x")
ARCHIVE_NAMES = {"thbgm.dat", "thbgm_tr.dat"}


class Track(NamedTuple):
    name: str
    offset: int
    intro: int
    length: int
    rate: int
    channels: int
    sample_bytes: int

    @property
    def frame_bytes(self):
        return self.channels * self.sample_bytes


def read_tracks(fmt, archive_size):
    records = fmt if isinstance(fmt, bytes) else fmt.read_bytes()
    tracks = {}
    position = 0
    while position < len(records) and records[position] != 0:
        if len(records) - position < TRACK_RECORD.size:
            raise ValueError("Truncated thbgm.fmt record.")
        name, offset, _, intro, length, codec, channels, rate, byte_rate, align, bits = (
            TRACK_RECORD.unpack_from(records, position))
        name = name.split(b"\0", 1)[0].decode("ascii").lower()
        if not re.fullmatch(r"[a-z0-9_-]+\.wav", name) or name in tracks:
            raise ValueError(f"Invalid or duplicate track name: {name}")
        if (codec != 1 or channels < 1 or bits not in (8, 16, 24, 32) or rate < 1
                or align != channels * (bits // 8) or byte_rate != rate * align
                or intro >= length or intro % align or length % align
                or offset + length > archive_size):
            raise ValueError(f"Invalid PCM format or archive range: {name}")
        tracks[name] = Track(name, offset, intro, length, rate, channels, bits // 8)
        position += TRACK_RECORD.size
    if not tracks or any(records[position:]):
        raise ValueError("Unrecognized or empty thbgm.fmt.")
    previous_end = 0
    for track in sorted(tracks.values(), key=lambda item: item.offset):
        if track.offset < previous_end:
            raise ValueError(f"Overlapping track ranges: {track.name}")
        previous_end = track.offset + track.length
    return tracks


def copy_pcm(source, write, track, fit):
    with wave.open(str(source), "rb") as audio:
        actual = (audio.getframerate(), audio.getnchannels(), audio.getsampwidth())
        expected = (track.rate, track.channels, track.sample_bytes)
        if actual != expected:
            raise ValueError(
                f"{source.name}: expected {track.rate} Hz, {track.channels} channel(s), "
                f"{track.sample_bytes * 8}-bit PCM.")
        frames = audio.getnframes()
        target = track.length // track.frame_bytes
        if not frames or (frames != target and not fit):
            raise ValueError(f"{source.name}: expected {target} frames, got {frames}. "
                             "Use --fit to repeat or trim the audio.")
        remaining = target
        while remaining:
            if audio.tell() == frames:
                audio.rewind()
            count = min(remaining, frames - audio.tell(), 65536)
            data = audio.readframes(count)
            if len(data) != count * track.frame_bytes:
                raise ValueError(f"Truncated WAV data: {source.name}")
            write(data)
            remaining -= count


def write_output(original, write):
    folder = original.parent / "thprac-bgm"
    output = folder / original.name
    folder.mkdir(exist_ok=True)
    if output.exists():
        raise FileExistsError(f"{output} already exists. Move it aside before rebuilding.")
    with tempfile.NamedTemporaryFile(dir=folder, suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
    try:
        with temporary.open("wb") as stream:
            write(stream)
        if os.name == "nt":
            temporary.rename(output)
        else:
            os.link(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    print(output)


def prepare_archive(args):
    archive = args.archive.absolute()
    if archive.name.lower() not in ARCHIVE_NAMES:
        raise ValueError("Pass the original thbgm.dat or thbgm_tr.dat.")
    tracks = read_tracks(args.fmt, archive.stat().st_size)
    if not args.wav:
        for track in tracks.values():
            print(f"{track.name}  {track.rate} Hz  {track.channels} ch  "
                  f"{track.sample_bytes * 8} bit  "
                  f"{track.length // track.frame_bytes} frames  "
                  f"loop starts at frame {track.intro // track.frame_bytes}")
        return
    replacements = {}
    for file in args.wav:
        name = file.name.lower()
        if name not in tracks or name in replacements:
            raise ValueError(f"Unknown or duplicate replacement track: {file.name}")
        replacements[name] = file

    def write(stream):
        with archive.open("rb") as original:
            shutil.copyfileobj(original, stream)
        for name, file in replacements.items():
            track = tracks[name]
            stream.seek(track.offset)
            copy_pcm(file, stream.write, track, args.fit)

    write_output(archive, write)


def prepare_wav(args):
    original = args.original.absolute()
    if original.parent.name.lower() != "bgm" or not re.fullmatch(
            r"th06_(0[1-9]|1[0-7])\.wav", original.name.lower()):
        raise ValueError("Pass an original TH06 WAV from the game's bgm folder.")
    with wave.open(str(original), "rb") as audio:
        rate, channels, sample_bytes = (audio.getframerate(), audio.getnchannels(), audio.getsampwidth())
        length = audio.getnframes() * channels * sample_bytes
    track = Track(original.name, 0, 0, length, rate, channels, sample_bytes)

    def write(stream):
        with wave.open(stream, "wb") as audio:
            audio.setparams((channels, sample_bytes, rate, 0, "NONE", "not compressed"))
            copy_pcm(args.wav, audio.writeframesraw, track, args.fit)

    write_output(original.parent.parent / original.name, write)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(required=True)
    wav = subcommands.add_parser("th06", help="Prepare a replacement for one TH06 WAV.")
    wav.add_argument("original", type=Path)
    wav.add_argument("wav", type=Path)
    wav.set_defaults(run=prepare_wav)
    archive = subcommands.add_parser("archive", help="List tracks or prepare a replacement archive.")
    archive.add_argument("archive", type=Path)
    archive.add_argument("fmt", type=Path)
    archive.add_argument("wav", type=Path, nargs="*", help="Replacement WAVs named for their tracks.")
    archive.set_defaults(run=prepare_archive)
    for command in (wav, archive):
        command.add_argument("--fit", action="store_true", help="Repeat or trim PCM to the original length.")
    args = parser.parse_args()
    try:
        args.run(args)
    except (OSError, ValueError, EOFError, wave.Error) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
