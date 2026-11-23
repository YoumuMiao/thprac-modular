"""Exercise production TH15 spell attribution against mock native records.

No game is launched. Unchanged chapter-record/save methods are checked against
the pre-change source; the extracted native methods run with synthetic memory.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", required=True, type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
source_path = root / "extensions/launcher-layout/thprac/src/thprac/thprac_th15.cpp"
source = source_path.read_text(encoding="utf-8-sig")


def block(text, marker, structure=False):
    start = text.index(marker)
    opened = text.index("{", start)
    depth = 1
    end = opened + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    if structure:
        end = text.index(";", end) + 1
    return text[start:end]


before_path = root.parent / "Release/versions/2.2.2.7/20261108-all-game-native-capture/build/evidence/th15-before-agent.cpp"
before = before_path.read_text(encoding="utf-8-sig")
for marker in ("void RecordCapture(", "void RecordChapterStats(", "void Save()"):
    assert block(source, marker) == block(before, marker), marker + " changed unexpectedly"
assert "int32_t version = 5;" in block(source, "void Save()")
for marker in ("th15_native_spell_capture,", "th15_native_spell_capture_total,"):
    assert "NativeSpellSucceeded" in source[source.index(marker):source.index(marker) + 320]
assert "th15_native_spell_success, 0x420e5b, 1" in source

locale = (source_path.parent / "thprac_locale_def.h").read_text(encoding="utf-8-sig")
enum = block(locale[locale.index("namespace TH15 {"):], "enum th_sections_t", True)
structs = "\n".join(block(source, "struct " + name, True)
                    for name in ("PendingCapture", "NativeSpellAttempt", "NativeSpellFinalize"))
methods = "\n".join(block(source, marker) for marker in (
    "void BeginNativeAttempt(", "void RememberNativeSpellAttempt(", "void QueueCapture(",
    "void ResolvePendingCaptures(", "void NativeSpellBefore(", "void NativeSpellSucceeded(",
    "void NativeSpellAfter(", "void SetCaptureRateEnabled("))
methods = re.sub(r"\*\(int32_t\*\)(0x[0-9a-fA-F]+)", r"GetMemContent<int32_t>(\1)", methods)
preamble = r'''
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>
using std::uintptr_t;
bool g_th15CaptureRateEnabled = true, g_th15NativeCaptureRateEnabled = true;
std::unordered_map<uintptr_t,uint32_t> mem;
template<class T> T GetMemContent(uintptr_t address){return static_cast<T>(mem[address]);}
namespace THPrac::TH15 {
'''
fixture = r'''
std::vector<PendingCapture> pendingCaptures;
std::vector<NativeSpellAttempt> nativeSpellAttempts;
uint64_t attemptSerial=0;
bool active=false, activeIsBossSection=false, activeObservedGameplay=false;
bool bossIntroPending=false,bossGapPending=false,failed=false,replayDisplayOnly=false;
bool captureRateEnabled=true,displayValid=false,captureRateResumePending=false;
struct { bool valid=false; } transitionSnapshot;
uint64_t transitionSnapshotUntil=0;
int activeMode=1,activeStage=0,activeChapter=0,activeSection=0,activeDiff=3,activePlayer=0;
int missesAtStart=0,bombsAtStart=0,grazeAtStart=0,killsAtStart=0,enemiesAtStart=0;
int scoreAtStart=0,activeBluePoints=0,maxPointAtStart=0;
using Key=std::tuple<int,int,int,int,int,bool>;
std::map<Key,int> captures;
std::vector<PendingCapture> statCalls;
void TraceNativeSpell(const char*,const NativeSpellFinalize&,uint32_t=0,bool=false)const{}
int ReadMaxPoint(){return GetMemContent<int>(0x4e7434);}
void RecordCapture(const PendingCapture& item){
 if(!g_th15CaptureRateEnabled)return;
 ++captures[{item.mode,item.stage,item.bossSection?item.section:10000+item.chapter,item.diff,item.player,item.replay}];
}
void RecordChapterStats(const PendingCapture& item){statCalls.push_back(item);}
int Count(int section,bool replay=false){return captures[{1,0,section,3,0,replay}];}
void Start(int section=TH15_ST1_BOSS2,bool spell=true,bool replay=false){
 BeginNativeAttempt();active=true;activeIsBossSection=spell;activeSection=section;
 activeStage=0;activeChapter=0;activeDiff=3;activePlayer=0;activeMode=1;
 activeObservedGameplay=true;bossIntroPending=bossGapPending=false;
 replayDisplayOnly=replay;failed=false;
 mem[0x4e9a70]=0x600000;mem[0x600078]=3;
 mem[0x600074]=TH15SpellCardId(section,3)-1;
 mem[0x4e9bc4]=0x800000;mem[0x80000c]=replay;
 mem[0x4e9bc8]=0x700000;mem[0x4e7404]=mem[0x4e7408]=0;mem[0x4e7794]=0;
}
'''
checks = r'''
int checks=0;
void Check(bool result){++checks;if(!result)throw checks;}
void Tests(){
 {Harness h;h.Start();h.QueueCapture(0,0);h.ResolvePendingCaptures(0,0);
  Check(h.Count(TH15_ST1_BOSS2)==0 && h.statCalls.size()==1);
  Check(h.pendingCaptures.empty() && h.nativeSpellAttempts.size()==1);}
 {Harness h;h.Start();h.NativeSpellBefore();Check(h.nativeSpellFinalize.armed);
  h.NativeSpellSucceeded();h.NativeSpellAfter();h.QueueCapture(1,1);h.ResolvePendingCaptures(1,1);
  Check(h.Count(TH15_ST1_BOSS2)==1 && h.statCalls.size()==1);
  h.NativeSpellAfter();h.NativeSpellBefore();h.NativeSpellSucceeded();h.NativeSpellAfter();
  Check(h.Count(TH15_ST1_BOSS2)==1);}
 {Harness h;h.Start();h.NativeSpellBefore();h.NativeSpellAfter();h.QueueCapture(0,0);h.ResolvePendingCaptures(0,0);
  Check(h.Count(TH15_ST1_BOSS2)==0);}
 {Harness h;h.Start();h.NativeSpellBefore();const auto p=h.nativeSpellFinalize.counter;mem[p]++;
  h.NativeSpellAfter();Check(h.Count(TH15_ST1_BOSS2)==1);}
 {Harness h;h.Start();h.NativeSpellBefore();const auto p=h.nativeSpellFinalize.counter;mem[p]=99999;
  h.NativeSpellBefore();h.NativeSpellSucceeded();h.NativeSpellAfter();Check(h.Count(TH15_ST1_BOSS2)==1);}
 {Harness h;h.Start();g_th15NativeCaptureRateEnabled=false;h.NativeSpellBefore();
  h.NativeSpellSucceeded();h.NativeSpellAfter();Check(h.Count(TH15_ST1_BOSS2)==1);
  g_th15NativeCaptureRateEnabled=true;}
 {Harness h;h.Start(TH15_ST1_BOSS2,true,true);h.NativeSpellBefore();h.NativeSpellSucceeded();h.NativeSpellAfter();
  Check(h.Count(TH15_ST1_BOSS2,true)==1 && h.Count(TH15_ST1_BOSS2,false)==0);}
 {Harness h;h.Start();mem[0x80000c]=1;h.NativeSpellBefore();Check(!h.nativeSpellFinalize.armed);}
 {Harness h;h.Start();mem[0x4e740c]=100;h.QueueCapture(0,0);h.ResolvePendingCaptures(0,0);
  const auto oldId=mem[0x600074];h.Start(TH15_ST1_BOSS3);h.failed=true;mem[0x600074]=oldId;mem[0x4e740c]=200;
  h.NativeSpellBefore();Check(h.nativeSpellFinalize.attempt.section==TH15_ST1_BOSS2);
  h.NativeSpellSucceeded();h.NativeSpellAfter();
  Check(h.Count(TH15_ST1_BOSS2)==1 && h.Count(TH15_ST1_BOSS3)==0 && h.failed);
  Check(h.statCalls.size()==1 && h.statCalls[0].scoreAtSettlement==100);}
 {Harness h;h.Start();h.NativeSpellBefore();h.NativeSpellAfter();h.Start(TH15_ST1_BOSS4);
  h.NativeSpellBefore();h.NativeSpellSucceeded();h.NativeSpellAfter();
  Check(h.Count(TH15_ST1_BOSS2)==0 && h.Count(TH15_ST1_BOSS4)==1);}
 {Harness h;h.Start();h.NativeSpellBefore();h.SetCaptureRateEnabled(false);h.SetCaptureRateEnabled(true);h.Start();
  h.NativeSpellAfter();Check(h.Count(TH15_ST1_BOSS2)==0 && h.nativeSpellAttempts.empty());
  h.NativeSpellBefore();h.NativeSpellSucceeded();h.NativeSpellAfter();Check(h.Count(TH15_ST1_BOSS2)==1);}
 {Harness h;h.Start(0,false);h.QueueCapture(0,0);h.ResolvePendingCaptures(0,0);Check(h.Count(10000)==1);
  h.Start(0,false);h.QueueCapture(0,0);h.ResolvePendingCaptures(1,0);Check(h.Count(10000)==1);}
 {Harness h;h.Start(TH15_ST1_BOSS1);h.QueueCapture(0,0);h.ResolvePendingCaptures(0,0);
  Check(h.Count(TH15_ST1_BOSS1)==1);}
 {Harness h;h.Start();mem[0x600074]=118;h.NativeSpellBefore();Check(!h.nativeSpellFinalize.armed);}
 {Harness h;h.Start();h.NativeSpellBefore();h.NativeSpellSucceeded();mem[0x4e9a70]=0x900000;
  h.NativeSpellAfter();Check(h.Count(TH15_ST1_BOSS2)==0);}
 {Harness h;h.Start();h.NativeSpellBefore();h.NativeSpellSucceeded();mem[0x600074]++;
  h.NativeSpellAfter();Check(h.Count(TH15_ST1_BOSS2)==0);}
 {Harness h;h.Start();g_th15CaptureRateEnabled=false;h.NativeSpellBefore();Check(!h.nativeSpellFinalize.armed);
  g_th15CaptureRateEnabled=true;}
 {Harness h;h.Start();h.NativeSpellBefore();
  Check(h.nativeSpellFinalize.counter==0x700000+0x5188+mem[0x600074]*0x9c+0x958);
  mem[0x4e7794]=0x120;h.NativeSpellBefore();
  Check(h.nativeSpellFinalize.counter==0x700000+mem[0x600074]*0x9c+0x958+4);}
}
}
int main(){try{THPrac::TH15::Tests();std::cout<<"PASS: "<<THPrac::TH15::checks<<" TH15 native spell event checks\n";}
 catch(int check){std::cerr<<"FAIL check "<<check<<'\n';return 1;}}
'''
code = preamble + enum + "\n" + block(source, "static int TH15SpellCardId(") + "\nstruct Harness {\n" + structs + "\n" + fixture + "\n" + methods + "\n};\n" + checks
(out / "test.cpp").write_text(code, encoding="utf-8")
vc = root.parent.parent / "thprac/build/toolchains/msvc-14.51-20261029/VC/Tools/MSVC/14.51.36231"
sdk = Path(r"C:\Program Files (x86)\Windows Kits\10")
version = "10.0.26100.0"
env = dict(os.environ)
env["PATH"] = str(vc / "bin/Hostx64/x86") + os.pathsep + env["PATH"]
env["INCLUDE"] = os.pathsep.join(str(x) for x in [vc / "include", sdk / "Include" / version / "ucrt"])
env["LIB"] = os.pathsep.join(str(x) for x in [vc / "lib/x86", sdk / "Lib" / version / "ucrt/x86", sdk / "Lib" / version / "um/x86"])
build = subprocess.run([str(vc / "bin/Hostx64/x86/cl.exe"), "/nologo", "/std:c++20", "/EHsc", "/utf-8", "/MT",
                        str(out / "test.cpp"), "/Fo" + str(out) + "\\", "/Fe" + str(out / "test.exe")],
                       cwd=out, env=env, capture_output=True)
(out / "compile.log").write_bytes(build.stdout + build.stderr)
if build.returncode:
    print((build.stdout + build.stderr).decode(errors="replace"))
    raise SystemExit(build.returncode)
test = subprocess.run([str(out / "test.exe")], cwd=out, env=env, capture_output=True)
(out / "result.log").write_bytes(test.stdout + test.stderr)
print((test.stdout + test.stderr).decode(errors="replace"))
raise SystemExit(test.returncode)
