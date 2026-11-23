// Local diagnostic watcher for this release's explicitly named private process.
// This tool never launches a process. Its caller owns resumption and cleanup.
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {
constexpr wchar_t buildRoot[] = L"C:\\Users\\dolph\\Documents\\Data\\thcode\\thprac-modular\\Release\\versions\\2.2.2.7\\20261108-all-game-native-capture\\build\\";
constexpr wchar_t runtimeRoot[] = L"C:\\Users\\dolph\\Documents\\Data\\thcode\\thprac-modular\\Release\\versions\\2.2.2.7\\20261108-all-game-native-capture\\build\\runtime\\";

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct DebugSession {
    DWORD pid = 0;
    bool attached = false;
    ~DebugSession() { if (attached) DebugActiveProcessStop(pid); }
};
std::wstring FullPath(const wchar_t* text)
{
    const DWORD length = GetFullPathNameW(text, 0, nullptr, nullptr);
    if (!length) return {};
    std::vector<wchar_t> buffer(length);
    const DWORD used = GetFullPathNameW(text, length, buffer.data(), nullptr);
    if (!used || used >= length) return {};
    std::wstring result(buffer.data(), used);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    if (result.starts_with(L"\\\\?\\")) result.erase(0, 4);
    return result;
}
bool Under(const std::wstring& path, const wchar_t* prefix)
{
    const size_t size = std::wcslen(prefix);
    return path.size() > size && _wcsnicmp(path.c_str(), prefix, size) == 0;
}
std::string Hex(uintptr_t value)
{
    char buffer[32];
    sprintf_s(buffer, "\"0x%08lx\"", static_cast<unsigned long>(value));
    return buffer;
}
std::string Number(uint64_t value) { return std::to_string(value); }
std::string Quote(const std::wstring& text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(length, '\0');
    if (length) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    std::string result = "\"";
    for (unsigned char ch : utf8) {
        if (ch == '\\' || ch == '"') { result += '\\'; result += static_cast<char>(ch); }
        else if (ch < 0x20) {
            char escaped[8]; sprintf_s(escaped, "\\u%04x", ch); result += escaped;
        } else result += static_cast<char>(ch);
    }
    return result + "\"";
}
struct Log {
    Handle file;
    bool failed = false;
    bool Line(const std::string& value)
    {
        const std::string line = value + "\n";
        DWORD written = 0;
        const bool ok = WriteFile(file.value, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)
            && written == line.size() && FlushFileBuffers(file.value);
        failed |= !ok;
        return ok;
    }
};
struct Module {
    uintptr_t base;
    DWORD size;
    std::wstring name, path;
};
std::vector<Module> Modules(DWORD pid)
{
    std::vector<Module> result;
    for (unsigned retry = 0; retry < 3; ++retry) {
        Handle snapshot {CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)};
        if (snapshot.value == INVALID_HANDLE_VALUE) { if (GetLastError() == ERROR_BAD_LENGTH) { Sleep(1); continue; } return result; }
        MODULEENTRY32W item {}; item.dwSize = sizeof(item);
        if (Module32FirstW(snapshot.value, &item)) do {
            result.push_back({reinterpret_cast<uintptr_t>(item.modBaseAddr), item.modBaseSize, item.szModule, item.szExePath});
        } while (result.size() < 256 && Module32NextW(snapshot.value, &item));
        return result;
    }
    return result;
}
bool IsNtdllAddress(DWORD pid, uintptr_t address)
{
    for (const auto& module : Modules(pid))
        if (_wcsicmp(module.name.c_str(), L"ntdll.dll") == 0
            && address >= module.base && address - module.base < module.size) return true;
    return false;
}
void LogModules(Log& log, DWORD pid)
{
    std::string line = "{\"event\":\"modules\",\"pid\":" + Number(pid) + ",\"items\":[";
    bool first = true;
    for (const auto& item : Modules(pid)) {
        if (!first) line += ',';
        first = false;
        line += "{\"base\":" + Hex(item.base) + ",\"size\":" + Number(item.size)
            + ",\"name\":" + Quote(item.name) + ",\"path\":" + Quote(item.path) + "}";
    }
    log.Line(line + "]}");
}
void LogException(Log& log, HANDLE process, const DEBUG_EVENT& event)
{
    const auto& info = event.u.Exception;
    const auto& record = info.ExceptionRecord;
    std::string line = "{\"event\":\"exception\",\"pid\":" + Number(event.dwProcessId)
        + ",\"tid\":" + Number(event.dwThreadId) + ",\"code\":" + Hex(record.ExceptionCode)
        + ",\"address\":" + Hex(reinterpret_cast<uintptr_t>(record.ExceptionAddress))
        + ",\"first_chance\":" + (info.dwFirstChance ? "true" : "false")
        + ",\"flags\":" + Hex(record.ExceptionFlags) + ",\"parameters\":[";
    for (DWORD i = 0; i < std::min<DWORD>(record.NumberParameters, EXCEPTION_MAXIMUM_PARAMETERS); ++i) {
        if (i) line += ',';
        line += Hex(record.ExceptionInformation[i]);
    }
    line += "]";
    Handle thread {OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, event.dwThreadId)};
    CONTEXT context {}; context.ContextFlags = CONTEXT_FULL;
    if (thread.value && GetProcessIdOfThread(thread.value) == event.dwProcessId && GetThreadContext(thread.value, &context)) {
        line += ",\"registers\":{\"eip\":" + Hex(context.Eip) + ",\"esp\":" + Hex(context.Esp)
            + ",\"ebp\":" + Hex(context.Ebp) + ",\"eax\":" + Hex(context.Eax) + ",\"ebx\":" + Hex(context.Ebx)
            + ",\"ecx\":" + Hex(context.Ecx) + ",\"edx\":" + Hex(context.Edx) + ",\"esi\":" + Hex(context.Esi)
            + ",\"edi\":" + Hex(context.Edi) + ",\"eflags\":" + Hex(context.EFlags) + "},\"stack\":[";
        DWORD stack[16] {}; SIZE_T got = 0;
        ReadProcessMemory(process, reinterpret_cast<const void*>(static_cast<uintptr_t>(context.Esp)), stack, sizeof(stack), &got);
        const size_t count = std::min<size_t>(got / sizeof(DWORD), 16);
        for (size_t i = 0; i < count; ++i) { if (i) line += ','; line += Hex(stack[i]); }
        line += "]";
    } else line += ",\"context_error\":" + Number(GetLastError());
    log.Line(line + "}");
}
void CloseEventHandles(const DEBUG_EVENT& event, HANDLE ownedProcess)
{
    if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
        if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
        if (event.u.CreateProcessInfo.hThread) CloseHandle(event.u.CreateProcessInfo.hThread);
        if (event.u.CreateProcessInfo.hProcess && event.u.CreateProcessInfo.hProcess != ownedProcess)
            CloseHandle(event.u.CreateProcessInfo.hProcess);
    } else if (event.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
        if (event.u.CreateThread.hThread) CloseHandle(event.u.CreateThread.hThread);
    } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
        if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
    }
}
int Run(int argc, wchar_t** argv)
{
    static_assert(sizeof(void*) == 4 && sizeof(DEBUG_EVENT) == 96, "Use the native Win32 debug-event layout");
    if (argc != 5) {
        std::fwprintf(stderr, L"Usage: native_capture_debug_watch.exe <pid> <expected-full-game-path> <new-output.jsonl> <new-ready-file>\n");
        return 1;
    }
    wchar_t* end = nullptr; errno = 0;
    const unsigned long parsed = std::wcstoul(argv[1], &end, 10);
    if (errno == ERANGE || !parsed || !end || end == argv[1] || *end) return 1;
    const DWORD pid = parsed;
    const auto expected = FullPath(argv[2]), output = FullPath(argv[3]), ready = FullPath(argv[4]);
    if (!Under(expected, runtimeRoot) || !Under(output, buildRoot) || !Under(ready, buildRoot)
        || _wcsicmp(output.c_str(), ready.c_str()) == 0) {
        std::fprintf(stderr, "Target must stay under this release's build/runtime; artifacts must stay under build.\n");
        return 2;
    }
    Handle process {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid)};
    if (!process.value) { std::fprintf(stderr, "OpenProcess failed: %lu\n", GetLastError()); return 2; }
    std::vector<wchar_t> path(32768); DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.value, 0, path.data(), &length)) return 2;
    const auto actual = FullPath(std::wstring(path.data(), length).c_str());
    if (_wcsicmp(actual.c_str(), expected.c_str()) != 0 || !Under(actual, runtimeRoot)
        || WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT) {
        std::fprintf(stderr, "PID image identity does not match the isolated expected path.\n");
        return 2;
    }
    Log log;
    log.file.value = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log.file.value == INVALID_HANDLE_VALUE) return 3;
    log.Line("{\"event\":\"identity\",\"pid\":" + Number(pid) + ",\"image\":" + Quote(actual) + "}");
    if (GetFileAttributesW(ready.c_str()) != INVALID_FILE_ATTRIBUTES) { log.Line("{\"event\":\"error\",\"message\":\"ready file exists\"}"); return 3; }
    DebugSession session {pid, false};
    if (!DebugActiveProcess(pid)) { log.Line("{\"event\":\"attach_error\",\"win32\":" + Number(GetLastError()) + "}"); return 4; }
    session.attached = true;
    if (!DebugSetProcessKillOnExit(FALSE)) { log.Line("{\"event\":\"kill_on_exit_error\",\"win32\":" + Number(GetLastError()) + "}"); return 4; }
    {
        Handle signal {CreateFileW(ready.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (signal.value == INVALID_HANDLE_VALUE) { log.Line("{\"event\":\"ready_error\",\"win32\":" + Number(GetLastError()) + "}"); return 3; }
        const std::string value = "{\"attached\":true,\"pid\":" + Number(pid) + "}\n";
        DWORD written = 0;
        if (!WriteFile(signal.value, value.data(), static_cast<DWORD>(value.size()), &written, nullptr)
            || written != value.size() || !FlushFileBuffers(signal.value)) return 3;
    }
    log.Line("{\"event\":\"attached\",\"pid\":" + Number(pid) + ",\"debug_kill_on_exit\":false}");
    std::printf("{\"event\":\"ready\",\"pid\":%lu}\n", pid); std::fflush(stdout);
    const ULONGLONG deadline = GetTickCount64() + 150000;
    bool initialBreakpoint = true;
    uint64_t gameBreakpoints = 0;
    const char* reason = "timeout";
    while (GetTickCount64() < deadline) {
        DEBUG_EVENT event {};
        if (!WaitForDebugEvent(&event, 100)) {
            if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
            log.Line("{\"event\":\"wait_error\",\"win32\":" + Number(GetLastError()) + "}");
            reason = "wait_error"; break;
        }
        DWORD continuation = DBG_CONTINUE;
        bool stop = false;
        if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const auto& info = event.u.Exception;
            const DWORD code = info.ExceptionRecord.ExceptionCode;
            const auto address = reinterpret_cast<uintptr_t>(info.ExceptionRecord.ExceptionAddress);
            continuation = DBG_EXCEPTION_NOT_HANDLED;
            if (code == EXCEPTION_BREAKPOINT && info.dwFirstChance && initialBreakpoint && IsNtdllAddress(pid, address)) {
                initialBreakpoint = false;
                continuation = DBG_CONTINUE;
                log.Line("{\"event\":\"attach_breakpoint\",\"tid\":" + Number(event.dwThreadId) + ",\"address\":" + Hex(address) + "}");
            } else if (code == EXCEPTION_BREAKPOINT && info.dwFirstChance) {
                ++gameBreakpoints; // Let thprac's VEH process every game INT3.
            } else if (code == 0x80000001u && info.dwFirstChance) {
                // Guard-page instrumentation is expected in the private game.
                // Preserve its exception handling without synchronous disk I/O
                // on every protected-page access.
            } else {
                LogException(log, process.value, event);
                if (!info.dwFirstChance || code == 0xC0000409u) {
                    LogModules(log, pid);
                    reason = "fatal_exception";
                    stop = true;
                }
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            log.Line("{\"event\":\"exit_process\",\"code\":" + Hex(event.u.ExitProcess.dwExitCode) + "}");
            reason = "process_exit"; stop = true;
        }
        CloseEventHandles(event, process.value);
        const bool continued = ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation) != FALSE;
        if (!continued) { log.Line("{\"event\":\"continue_error\",\"win32\":" + Number(GetLastError()) + "}"); reason = "continue_error"; break; }
        if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) session.attached = false;
        if (stop || log.failed) break;
    }
    log.Line("{\"event\":\"finished\",\"reason\":\"" + std::string(reason)
        + "\",\"game_breakpoints_passed_to_veh\":" + Number(gameBreakpoints) + "}");
    if (session.attached) {
        const bool detached = DebugActiveProcessStop(pid) != FALSE;
        const DWORD error = GetLastError();
        log.Line("{\"event\":\"detached\",\"ok\":" + std::string(detached ? "true" : "false") + ",\"win32\":" + Number(detached ? 0 : error) + "}");
        if (detached || WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) session.attached = false;
    }
    return log.failed ? 3 : 0;
}
} // namespace
int wmain(int argc, wchar_t** argv)
{
    try { return Run(argc, argv); }
    catch (...) { std::fprintf(stderr, "Scoped debug watcher exception; detach guard executed.\n"); return 9; }
}
