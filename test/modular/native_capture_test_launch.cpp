// Deterministic isolated-game launcher for native capture tests.
// The parent supplies an isolated game tree, APPDATA and a production Request.
// No production source executable is patched and this helper never injects itself.

#define NOMINMAX
#include <Windows.h>
#include "../../extensions/quick-practice/service.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

using namespace THPrac::QuickPractice;

namespace {
struct SharedRequest {
    volatile LONG status;
    Request request;
};
static_assert(sizeof(void*) == 4 && sizeof(Request) == 540 && sizeof(SharedRequest) == 544);

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct OwnedProcess {
    PROCESS_INFORMATION info {};
    bool leaveRunning = false;
    ~OwnedProcess()
    {
        if (info.hProcess && !leaveRunning && WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
            TerminateProcess(info.hProcess, ERROR_CANCELLED);
            WaitForSingleObject(info.hProcess, 5000);
        }
        if (info.hThread) CloseHandle(info.hThread);
        if (info.hProcess) CloseHandle(info.hProcess);
    }
};

struct Mapping {
    Handle handle;
    SharedRequest* view = nullptr;
    ~Mapping() { if (view) UnmapViewOfFile(view); }
};

DWORD createdPid = 0;

int Fail(int code, const char* message, DWORD error = GetLastError())
{
    std::printf("{\"event\":\"error\",\"pid\":%lu,\"code\":%d,\"win32\":%lu,\"message\":\"%s\"}\n",
        static_cast<unsigned long>(createdPid), code, static_cast<unsigned long>(error), message);
    std::fflush(stdout);
    std::fprintf(stderr, "%s (Win32=%lu)\n", message, static_cast<unsigned long>(error));
    return code;
}

std::wstring FullPath(const wchar_t* input)
{
    const DWORD needed = GetFullPathNameW(input, 0, nullptr, nullptr);
    if (!needed) return {};
    std::vector<wchar_t> path(needed);
    const DWORD used = GetFullPathNameW(input, needed, path.data(), nullptr);
    if (!used || used >= needed) return {};
    return std::wstring(path.data(), used);
}

bool RegularFile(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool LoadOILP(HANDLE process, const std::wstring& dll, DWORD& module)
{
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, dll.c_str(), bytes, &written) || written != bytes) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        SetLastError(error ? error : ERROR_WRITE_FAULT);
        return false;
    }
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!loadLibrary) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        SetLastError(error);
        return false;
    }
    Handle thread {CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr)};
    if (!thread.value) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        SetLastError(error);
        return false;
    }
    const DWORD wait = WaitForSingleObject(thread.value, 20000);
    if (wait != WAIT_OBJECT_0) {
        // Do not free a path that an unfinished remote thread may still read.
        // OwnedProcess terminates this new test game on the failure path.
        SetLastError(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        return false;
    }
    const bool obtained = GetExitCodeThread(thread.value, &module) != FALSE;
    const DWORD error = GetLastError();
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    if (!obtained || !module) {
        SetLastError(obtained ? ERROR_DLL_INIT_FAILED : error);
        return false;
    }
    return true;
}

LONG StatusValue(SharedRequest* request)
{
    return InterlockedCompareExchange(&request->status, 0, 0);
}
} // namespace

int Run(int argc, wchar_t** argv)
{
    if (argc != 4) {
        std::fwprintf(stderr, L"Usage: native_capture_test_launch.exe <host-thprac.exe> <game.exe> <request540.bin>\n");
        return 1;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const std::wstring host = FullPath(argv[1]);
    const std::wstring game = FullPath(argv[2]);
    const std::wstring packetPath = FullPath(argv[3]);
    if (host.empty() || game.empty() || packetPath.empty() || !RegularFile(host) || !RegularFile(game))
        return Fail(1, "Invalid host or game path", ERROR_FILE_NOT_FOUND);
    const auto slash = game.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return Fail(1, "Game directory is unavailable", ERROR_PATH_NOT_FOUND);
    const std::wstring directory = game.substr(0, slash);
    const std::wstring oilp = directory + L"\\openinputlagpatch.dll";
    if (!RegularFile(oilp)) return Fail(1, "The isolated OILP DLL is missing", ERROR_FILE_NOT_FOUND);

    Handle input {CreateFileW(packetPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (input.value == INVALID_HANDLE_VALUE) return Fail(2, "Cannot open Request packet");
    LARGE_INTEGER packetSize {};
    Request request {};
    DWORD read = 0;
    if (!GetFileSizeEx(input.value, &packetSize) || packetSize.QuadPart != sizeof(request)
        || !ReadFile(input.value, &request, sizeof(request), &read, nullptr) || read != sizeof(request))
        return Fail(2, "Request file must contain exactly 540 bytes", ERROR_INVALID_DATA);
    if (request.version != protocol || !std::memchr(request.game, 0, sizeof(request.game))
        || request.bytes < 16 || request.bytes > sizeof(request.parameters))
        return Fail(2, "Invalid Request protocol or parameter length", ERROR_INVALID_DATA);

    OwnedProcess gameProcess;
    STARTUPINFOW startup {sizeof(startup)};
    std::wstring gameCommand = L"\"" + game + L"\"";
    if (!CreateProcessW(game.c_str(), gameCommand.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED, nullptr, directory.c_str(), &startup, &gameProcess.info))
        return Fail(3, "Cannot create the suspended isolated game");
    createdPid = gameProcess.info.dwProcessId;

    FILETIME creation {}, exit {}, kernel {}, user {};
    if (!GetProcessTimes(gameProcess.info.hProcess, &creation, &exit, &kernel, &user))
        return Fail(4, "Cannot read isolated process creation time");
    wchar_t mappingName[96];
    if (swprintf_s(mappingName, L"Local\\THPrac.QuickPractice.%lu.%08lx%08lx",
            static_cast<unsigned long>(createdPid), creation.dwHighDateTime, creation.dwLowDateTime) < 0)
        return Fail(4, "Cannot format mapping identity", ERROR_INVALID_DATA);
    Mapping mapping;
    mapping.handle.value = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(SharedRequest), mappingName);
    const DWORD mappingError = GetLastError();
    if (!mapping.handle.value || mappingError == ERROR_ALREADY_EXISTS)
        return Fail(4, "Cannot create a unique Quick Practice mapping", mappingError);
    mapping.view = static_cast<SharedRequest*>(MapViewOfFile(mapping.handle.value, FILE_MAP_WRITE,
        0, 0, sizeof(SharedRequest)));
    if (!mapping.view) return Fail(4, "Cannot map Quick Practice request");
    std::memset(mapping.view, 0, sizeof(*mapping.view));
    mapping.view->request = request;
    InterlockedExchange(&mapping.view->status, static_cast<LONG>(Status::Pending));
    MemoryBarrier();
    std::printf("{\"event\":\"created\",\"pid\":%lu,\"mapping\":\"Local\\\\THPrac.QuickPractice.%lu.%08lx%08lx\",\"request_bytes\":540}\n",
        static_cast<unsigned long>(createdPid), static_cast<unsigned long>(createdPid),
        creation.dwHighDateTime, creation.dwLowDateTime);
    std::fflush(stdout);

    wchar_t debugReady[32768] {};
    if (GetEnvironmentVariableW(L"THPRAC_TEST_DEBUG_READY", debugReady, 32768)) {
        const ULONGLONG debugDeadline = GetTickCount64() + 20000;
        while (GetFileAttributesW(debugReady) == INVALID_FILE_ATTRIBUTES) {
            if (GetTickCount64() >= debugDeadline)
                return Fail(4, "Scoped debugger did not become ready", ERROR_TIMEOUT);
            Sleep(10);
        }
    }

    DWORD oilpModule = 0;
    if (!LoadOILP(gameProcess.info.hProcess, oilp, oilpModule))
        return Fail(5, "Cannot initialize OILP in the isolated game");
    std::printf("{\"event\":\"oilp_loaded\",\"pid\":%lu,\"module\":%lu}\n",
        static_cast<unsigned long>(createdPid), static_cast<unsigned long>(oilpModule));
    std::fflush(stdout);

    OwnedProcess attachment;
    STARTUPINFOW attachStartup {sizeof(attachStartup)};
    attachStartup.dwFlags = STARTF_USESHOWWINDOW;
    attachStartup.wShowWindow = SW_HIDE;
    std::wstring attachCommand = L"\"" + host + L"\" --attach " + std::to_wstring(createdPid);
    if (!CreateProcessW(host.c_str(), attachCommand.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, directory.c_str(), &attachStartup, &attachment.info))
        return Fail(6, "Cannot start the supplied thprac attachment host");
    const DWORD attachWait = WaitForSingleObject(attachment.info.hProcess, 30000);
    if (attachWait != WAIT_OBJECT_0)
        return Fail(6, "Timed out waiting for the thprac attachment host",
            attachWait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    DWORD attachExit = 0;
    if (!GetExitCodeProcess(attachment.info.hProcess, &attachExit) || attachExit != 0)
        return Fail(6, "The thprac attachment host failed", attachExit ? attachExit : GetLastError());
    std::printf("{\"event\":\"attach_done\",\"pid\":%lu,\"exit_code\":0}\n", static_cast<unsigned long>(createdPid));
    std::fflush(stdout);

    if (ResumeThread(gameProcess.info.hThread) == static_cast<DWORD>(-1))
        return Fail(7, "Cannot resume the isolated game");
    const ULONGLONG deadline = GetTickCount64() + 120000;
    LONG previous = -1;
    while (GetTickCount64() < deadline) {
        const LONG status = StatusValue(mapping.view);
        if (status != previous) {
            std::printf("{\"event\":\"status\",\"pid\":%lu,\"status\":%ld}\n",
                static_cast<unsigned long>(createdPid), status);
            std::fflush(stdout);
            previous = status;
        }
        if (status == static_cast<LONG>(Status::Entered)) {
            gameProcess.leaveRunning = true;
            std::printf("{\"event\":\"entered\",\"pid\":%lu,\"status\":4}\n", static_cast<unsigned long>(createdPid));
            std::fflush(stdout);
            return 0;
        }
        if (status == static_cast<LONG>(Status::Rejected) || status == static_cast<LONG>(Status::Cancelled))
            return Fail(8, "Quick Practice rejected or cancelled the request", ERROR_INVALID_DATA);
        if (WaitForSingleObject(gameProcess.info.hProcess, 0) == WAIT_OBJECT_0) {
            DWORD gameExit = 0;
            GetExitCodeProcess(gameProcess.info.hProcess, &gameExit);
            return Fail(8, "The isolated game exited before Quick Practice entry", gameExit);
        }
        Sleep(10);
    }
    return Fail(8, "Timed out waiting for Quick Practice Entered", ERROR_TIMEOUT);
}

int wmain(int argc, wchar_t** argv)
{
    try {
        return Run(argc, argv);
    } catch (...) {
        // Run's owned process guards unwind before reaching this handler.
        return Fail(9, "Unexpected isolated-launcher exception", ERROR_UNHANDLED_EXCEPTION);
    }
}
