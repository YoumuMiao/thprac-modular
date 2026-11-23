"""Build the actual capture runtime with an isolated store and test native event attribution."""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / ".thprac_data").mkdir(exist_ok=True)
source_dir = root / "extensions/launcher-layout/thprac/src/thprac"
core = (source_dir / "thprac_boss_capture.cpp").read_text(encoding="utf-8-sig")
assert core.count("static Runtime runtime;") == 1
core = core.replace("static Runtime runtime;", 'static Runtime runtime(L"native-event-store.dat");')
(out / "core.cpp").write_text(core, encoding="utf-8")
vc = root.parent.parent / "thprac/build/toolchains/msvc-14.51-20261029/VC/Tools/MSVC/14.51.36231"
sdk = Path(r"C:\Program Files (x86)\Windows Kits\10")
version = "10.0.26100.0"
env = dict(os.environ)
env["PATH"] = str(vc / "bin/Hostx64/x86") + os.pathsep + env["PATH"]
env["INCLUDE"] = os.pathsep.join(str(x) for x in [vc / "include", *[sdk / "Include" / version / x for x in ("ucrt", "shared", "um")]])
env["LIB"] = os.pathsep.join(str(x) for x in [vc / "lib/x86", sdk / "Lib" / version / "ucrt/x86", sdk / "Lib" / version / "um/x86"])
command = [str(vc / "bin/Hostx64/x86/cl.exe"), "/nologo", "/std:c++20", "/EHsc", "/utf-8", "/MT",
           "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(source_dir), str(Path(__file__).with_suffix(".cpp")),
           str(out / "core.cpp"), "/Fo" + str(out) + "\\", "/Fe" + str(out / "test.exe"), "/link", "shell32.lib"]
built = subprocess.run(command, cwd=out, env=env, capture_output=True)
(out / "compile.log").write_bytes(built.stdout + built.stderr)
if built.returncode:
    print((built.stdout + built.stderr).decode(errors="replace"))
    raise SystemExit(built.returncode)
test = subprocess.run([str(out / "test.exe")], cwd=out, env=env, capture_output=True)
(out / "result.log").write_bytes(test.stdout + test.stderr)
print((test.stdout + test.stderr).decode(errors="replace"))
raise SystemExit(test.returncode)
