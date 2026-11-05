#define NOMINMAX

#include "thprac_launcher_games.h"
#include "thprac_gui_components.h"
#include "thprac_gui_locale.h"
#include "thprac_launcher_cfg.h"
#include "thprac_launcher_cover.h"
#include "thprac_launcher_games_def.h"
#include "thprac_launcher_main.h"
#include "thprac_launcher_utils.h"
#include "thprac_launcher_wnd.h"
#include "thprac_load_exe.h"
#include "thprac_main.h"
#include "thprac_utils.h"
#include <Windows.h>
#include <shlwapi.h>
#include <cstdint>
#include <functional>
#include <imgui.h>
#include <metrohash128.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <vector>
#include <unordered_map>
#include <format>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cwctype>

#define ZLIB_WINAPI
#include "../3rdParties/minizip/include/unzip.h"
#include "../3rdParties/minizip/include/zip.h"
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "zlibstat.lib")
#pragma comment(lib, "zlibwapi.lib")

namespace THPrac {

bool AddfiletoZip(zipFile zfile, const std::string& fileNameinZip, const std::string& srcfile)
{
    int nErr = 0;
    zip_fileinfo zinfo = { 0 };
    tm_zip tmz = { 0 };
    zinfo.tmz_date = tmz;
    zinfo.dosDate = 0;
    zinfo.internal_fa = 0;
    zinfo.external_fa = 0;
    char sznewfileName[MAX_PATH] = { 0 };
    memset(sznewfileName, 0x00, sizeof(sznewfileName));
    strcat_s(sznewfileName, fileNameinZip.c_str());
    if (srcfile.empty()) {
        strcat_s(sznewfileName, "\\");
    }
    nErr = zipOpenNewFileInZip(zfile, sznewfileName, &zinfo, NULL, 0, NULL, 0, NULL, Z_DEFLATED, Z_DEFAULT_COMPRESSION);
    if (nErr != ZIP_OK) {
        return false;
    }
    if (!srcfile.empty()) {
        FILE* srcfp = _fsopen(srcfile.c_str(), "rb", _SH_DENYNO);
        if (NULL == srcfp) {
            return false;
        }
        int numBytes = 0;
        char* pBuf = new char[1024 * 100];
        if (NULL == pBuf) {
            return 0;
        }
        while (!feof(srcfp)) {
            memset(pBuf, 0x00, sizeof(pBuf));
            numBytes = fread(pBuf, 1, sizeof(pBuf), srcfp);
            nErr = zipWriteInFileInZip(zfile, pBuf, numBytes);
            if (ferror(srcfp)) {
                break;
            }
        }
        delete[] pBuf;
        fclose(srcfp);
    }
    zipCloseFileInZip(zfile);
    return true;
}

inline bool HashCompare(uint32_t hash1[4], uint32_t hash2[4])
{
    for (int i = 0; i < 4; ++i) {
        if (hash1[i] != hash2[i]) {
            return false;
        }
    }
    return true;
}
inline bool OepCompare(uint32_t oepCode1[10], uint32_t opeCode2[10])
{
    for (int i = 0; i < 10; ++i) {
        if (oepCode1[i] != opeCode2[i]) {
            return false;
        }
    }
    return true;
}
bool ReadMemory(void* buffer, void* addr, size_t size)
{
    SIZE_T bytesRead = 0;
    ReadProcessMemory(GetCurrentProcess(), addr, buffer, size, &bytesRead);
    return bytesRead == size;
}
bool GetExeInfo(void* exeBuffer, size_t exeSize, ExeSig& exeSigOut)
{
    if (exeSize < 128)
        return false;

    IMAGE_DOS_HEADER dosHeader;
    if (!ReadMemory(&dosHeader, exeBuffer, sizeof(IMAGE_DOS_HEADER)) || dosHeader.e_magic != 0x5a4d)
        return false;
    IMAGE_NT_HEADERS ntHeader;
    if (!ReadMemory(&ntHeader, (void*)((DWORD)exeBuffer + dosHeader.e_lfanew), sizeof(IMAGE_NT_HEADERS)) || ntHeader.Signature != 0x00004550)
        return false;

    exeSigOut.timeStamp = ntHeader.FileHeader.TimeDateStamp;
    exeSigOut.textSize = 0;
    for (auto& codeBlock : exeSigOut.oepCode) {
        codeBlock = 0;
    }
    for (auto& hashBlock : exeSigOut.metroHash) {
        hashBlock = 0;
    }

    PIMAGE_SECTION_HEADER pSection = (PIMAGE_SECTION_HEADER)((ULONG_PTR)((LONG)exeBuffer + dosHeader.e_lfanew) + ((LONG)(LONG_PTR) & (((IMAGE_NT_HEADERS*)0)->OptionalHeader)) + ntHeader.FileHeader.SizeOfOptionalHeader);
    for (int i = 0; i < ntHeader.FileHeader.NumberOfSections; i++, pSection++) {
        IMAGE_SECTION_HEADER section;
        if (!ReadMemory(&section, pSection, sizeof(IMAGE_SECTION_HEADER)))
            continue;
        if (!strcmp(".text", (char*)section.Name)) {
            exeSigOut.textSize = section.SizeOfRawData;
        }
        DWORD pOepCode = ntHeader.OptionalHeader.AddressOfEntryPoint;
        if (pOepCode >= section.VirtualAddress && pOepCode <= (section.VirtualAddress + section.Misc.VirtualSize)) {
            pOepCode -= section.VirtualAddress;
            pOepCode += section.PointerToRawData;
            pOepCode += (DWORD)exeBuffer;

            uint16_t oepCode[10];
            if (!ReadMemory(&oepCode, (void*)pOepCode, sizeof(oepCode)))
                continue;

            for (unsigned int j = 0; j < 10; ++j) {
                exeSigOut.oepCode[j] = (uint32_t) * (oepCode + j);
                exeSigOut.oepCode[j] ^= (j + 0x41) | ((j + 0x41) << 8);
            }
        }
    }

    if (exeSize < (1 << 23)) {
        MetroHash128::Hash((uint8_t*)exeBuffer, exeSize, (uint8_t*)exeSigOut.metroHash);
    }

    return true;
}
bool GetExeInfoEx(uintptr_t hProcess, uintptr_t base, ExeSig& exeSigOut)
{
    DWORD bytesRead;
    HANDLE hProc = (HANDLE)hProcess;

    IMAGE_DOS_HEADER dosHeader;
    if (!ReadProcessMemory(hProc, (void*)base, &dosHeader, sizeof(IMAGE_DOS_HEADER), &bytesRead)) {
        return false;
    }
    IMAGE_NT_HEADERS ntHeader;
    if (!ReadProcessMemory(hProc, (void*)(base + dosHeader.e_lfanew), &ntHeader, sizeof(IMAGE_NT_HEADERS), &bytesRead)) {
        return false;
    }

    exeSigOut.timeStamp = ntHeader.FileHeader.TimeDateStamp;
    PIMAGE_SECTION_HEADER pSection = (PIMAGE_SECTION_HEADER)((ULONG_PTR)(base + dosHeader.e_lfanew) + ((LONG)(LONG_PTR) & (((IMAGE_NT_HEADERS*)0)->OptionalHeader)) + ntHeader.FileHeader.SizeOfOptionalHeader);
    for (int i = 0; i < ntHeader.FileHeader.NumberOfSections; i++, pSection++) {
        IMAGE_SECTION_HEADER section;
        if (!ReadProcessMemory(hProc, (void*)(pSection), &section, sizeof(IMAGE_SECTION_HEADER), &bytesRead)) {
            return false;
        }
        if (!strcmp(".text", (char*)section.Name)) {
            exeSigOut.textSize = section.SizeOfRawData;
            return true;
        }
    }

    return false;
}
bool IfEndWith(const char* str, const char* subStr)
{
    auto subStrLen = strlen(subStr);
    auto strLen = strlen(str);

    if (strLen < subStrLen)
        return false;
    for (size_t i = 1; i <= subStrLen; i++) {
        if (str[strLen - i] != subStr[subStrLen - i])
            return false;
    }
    return true;
}
bool LoadJsonFile(std::wstring& path, void*& buffer, size_t& size)
{
    DWORD openFlag = OPEN_EXISTING;
    DWORD openAccess = GENERIC_READ;
    auto hFile = CreateFileW(path.c_str(), openAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, openFlag, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytesProcessed;
    auto fileSize = GetFileSize(hFile, nullptr);
    auto fileBuffer = malloc(fileSize + 1);
    memset(fileBuffer, 0, fileSize + 1);
    if (!ReadFile(hFile, fileBuffer, fileSize, &bytesProcessed, nullptr)) {
        CloseHandle(hFile);
        free(fileBuffer);
        return false;
    }

    CloseHandle(hFile);
    buffer = fileBuffer;
    size = fileSize;
    return true;
}
bool CheckIsValidJson(std::wstring& jsonPath)
{
    void* fileBuffer;
    size_t fileSize;

    if (LoadJsonFile(jsonPath, fileBuffer, fileSize)) {
        rapidjson::Document json;
        if (!(json.Parse((const char*)fileBuffer, fileSize + 1).HasParseError()) && json.IsObject()) {
            free(fileBuffer);
            return true;
        }
        free(fileBuffer);
    }

    return false;
}
int CheckHasSteamDRM(void* exeBuffer, size_t exeSize)
{
    if (exeSize < 128)
        return 0;

    IMAGE_DOS_HEADER dosHeader;
    if (!ReadMemory(&dosHeader, exeBuffer, sizeof(IMAGE_DOS_HEADER)) || dosHeader.e_magic != 0x5a4d)
        return 0;
    IMAGE_NT_HEADERS ntHeader;
    if (!ReadMemory(&ntHeader, (void*)((DWORD)exeBuffer + dosHeader.e_lfanew), sizeof(IMAGE_NT_HEADERS)) || ntHeader.Signature != 0x00004550)
        return 0;

    PIMAGE_SECTION_HEADER pSection = (PIMAGE_SECTION_HEADER)((ULONG_PTR)((LONG)exeBuffer + dosHeader.e_lfanew) + ((LONG)(LONG_PTR) & (((IMAGE_NT_HEADERS*)0)->OptionalHeader)) + ntHeader.FileHeader.SizeOfOptionalHeader);
    for (int i = 0; i < ntHeader.FileHeader.NumberOfSections; i++, pSection++) {
        IMAGE_SECTION_HEADER section;
        if (!ReadMemory(&section, pSection, sizeof(IMAGE_SECTION_HEADER)))
            continue;
        if (!strcmp(".bind", (char*)section.Name)) {
            static std::string_view ver2_0Sig("\x53\x51\x52\x56\x57\x55\x8B\xEC\x81\xEC\x00\x10\x00\x00\xBE", 15);
            static std::string_view ver2_1Sig("\x53\x51\x52\x56\x57\x55\x8B\xEC\x81\xEC\x00\x10\x00\x00\xC7", 15);
            static std::string_view ver3Sig("\xE8\x00\x00\x00\x00\x50\x53\x51\x52\x56\x57\x55\x8B\x44\x24\x1C\x2D\x05\x00\x00\x00\x8B\xCC\x83\xE4\xF0\x51\x51\x51\x50", 30);
            static std::string_view ver3PatternHead("\x55\x8B\xEC\x81\xEC", 5);
            static std::string_view ver3Sub1_1("\x53", 1);
            static std::string_view ver3Sub1_2("\x68", 1);
            static std::string_view ver3Sub2_1("\x53", 1);
            static std::string_view ver3Sub2_2("\x8D\x83", 2);
            static std::string_view ver3Sub3_1("\x56", 1);
            static std::string_view ver3Sub3_2("\x8D", 1);
            std::string_view exeStr((char*)exeBuffer, exeSize);

            if (exeStr.find(ver2_0Sig, section.PointerToRawData) != std::string_view::npos) {
                return 2;
            } else if (exeStr.find(ver2_0Sig, section.PointerToRawData) != std::string_view::npos) {
                return 2;
            } else if (exeStr.find(ver3Sig, section.PointerToRawData) != std::string_view::npos) {
                auto pos = exeStr.find(ver3PatternHead, section.PointerToRawData);
                uint32_t headerSize = 0;
                if (pos != std::string_view::npos) {
                    if (exeStr.find(ver3Sub1_1, pos) == pos + 9 && exeStr.find(ver3Sub1_2, pos) == pos + 15) {
                        headerSize = *(uint32_t*)((uint32_t)exeBuffer + pos + 0x10);
                    } else if (exeStr.find(ver3Sub2_1, pos) == pos + 9 && exeStr.find(ver3Sub2_2, pos) == pos + 15) {
                        headerSize = *(uint32_t*)((uint32_t)exeBuffer + pos + 0x16);
                    } else if (exeStr.find(ver3Sub3_1, pos) == pos + 9 && exeStr.find(ver3Sub3_2, pos) == pos + 20) {
                        headerSize = *(uint32_t*)((uint32_t)exeBuffer + pos + 0x10);
                    }
                }
                if (headerSize == 0xb0 || headerSize == 0xd0 || headerSize == 0xf0) {
                    return 3;
                }
            }
        }
    }

    return 0;
}

struct THGameInst {
    std::string name;
    std::string path;
    THGameType type;

    bool useVpatch = true;
    bool useOILP = false;
    bool useTHPrac = false;
    bool autoBackup = false;

    THGameInst() = default;
    THGameInst(const char* _name, std::string& _path, THGameType _type)
    {
        name = _name;
        path = _path;
        type = _type;
    }
};
struct THGameScan {
    THGameInst game;
    THGameSig* signature;
    bool checked;

    THGameScan(const char* _name, const char* _path, THGameType _type, THGameSig* _signature, bool _checked = true)
    {
        game.name = _name;
        game.path = _path;
        game.type = _type;
        signature = _signature;
        checked = _checked;
    }
};
struct THGame {
    THGameSig signature;
    std::vector<THGameInst> instances;
    int selected = 0;
    int defaultLaunch = -1;
};

struct THScoreSaveManagerPaths {
    std::wstring activeSave;
    std::wstring slotDirectory;
};

static bool THIsRegularFile(const std::wstring& path)
{
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring THScoreSaveAppDataRoot()
{
    wchar_t appData[MAX_PATH] = {};
    const auto length = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return {};
    return std::wstring(appData) + L"\\ShanghaiAlice";
}

static THScoreSaveManagerPaths THGetScoreSaveManagerPaths(
    const THGameSig& game, const THGameInst& instance)
{
    THScoreSaveManagerPaths paths;
    const auto fileName = utf8_to_utf16(game.scoreFileStr);
    const auto gameId = utf8_to_utf16(game.idStr);
    const auto appDataRoot = THScoreSaveAppDataRoot();
    const auto appDataSave = appDataRoot.empty() ? std::wstring()
        : appDataRoot + L"\\" + gameId + L"\\" + fileName;
    const auto legacyAppDataSave = appDataRoot.empty() ? std::wstring()
        : appDataRoot + L"\\" + fileName;
    const auto gameDirectory = GetDirFromFullPath(utf8_to_utf16(instance.path.c_str()));
    const auto gameRootSave = gameDirectory + fileName;

    if (!appDataSave.empty() && THIsRegularFile(appDataSave))
        paths.activeSave = appDataSave;
    else if (!legacyAppDataSave.empty() && THIsRegularFile(legacyAppDataSave))
        paths.activeSave = legacyAppDataSave;
    else if (THIsRegularFile(gameRootSave))
        paths.activeSave = gameRootSave;
    else if (game.appdataStr && !appDataSave.empty())
        paths.activeSave = appDataSave;
    else
        paths.activeSave = gameRootSave;

    if (!appDataRoot.empty())
        paths.slotDirectory = appDataRoot + L"\\thprac\\save_slots\\" + gameId;
    return paths;
}

static bool THEnsureDirectoryTree(const std::wstring& path)
{
    if (path.empty())
        return false;
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    const auto separator = path.find_last_of(L"\\/");
    if (separator != std::wstring::npos && separator > 2
        && !THEnsureDirectoryTree(path.substr(0, separator)))
        return false;
    if (CreateDirectoryW(path.c_str(), nullptr))
        return true;
    return GetLastError() == ERROR_ALREADY_EXISTS
        && (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static std::vector<std::wstring> THListScoreSaveSlots(const std::wstring& directory)
{
    std::vector<std::wstring> result;
    if (directory.empty())
        return result;
    WIN32_FIND_DATAW findData {};
    const auto pattern = directory + L"\\*.dat";
    const auto find = FindFirstFileW(pattern.c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE)
        return result;
    do {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            result.emplace_back(findData.cFileName);
    } while (FindNextFileW(find, &findData));
    FindClose(find);
    std::sort(result.begin(), result.end());
    return result;
}

static std::wstring THScoreSaveSlotStem(const std::wstring& filename)
{
    const auto dot = filename.find_last_of(L'.');
    return dot == std::wstring::npos ? filename : filename.substr(0, dot);
}

static std::wstring THSanitizeScoreSaveName(const char* name)
{
    std::wstring result = utf8_to_utf16(name ? name : "");
    for (auto& ch : result) {
        if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch))
            ch = L'_';
    }
    while (!result.empty() && (result.back() == L' ' || result.back() == L'.'))
        result.pop_back();
    while (!result.empty() && (result.front() == L' ' || result.front() == L'.'))
        result.erase(result.begin());
    if (result.empty() || result.size() > 80 || result == L"." || result == L"..")
        return {};

    std::wstring deviceName = result;
    const auto dot = deviceName.find(L'.');
    if (dot != std::wstring::npos)
        deviceName.resize(dot);
    for (auto& ch : deviceName)
        ch = static_cast<wchar_t>(towupper(ch));
    if (deviceName == L"CON" || deviceName == L"PRN" || deviceName == L"AUX"
        || deviceName == L"NUL" || (deviceName.size() == 4
            && (deviceName.starts_with(L"COM") || deviceName.starts_with(L"LPT"))
            && deviceName[3] >= L'1' && deviceName[3] <= L'9'))
        return {};
    return result;
}

static std::wstring THGetScoreSaveSlotPath(const std::wstring& directory,
    const std::wstring& filename)
{
    return directory.empty() ? std::wstring() : directory + L"\\" + filename;
}

static std::wstring THMakeScoreSaveSwitchBackupName()
{
    SYSTEMTIME now {};
    GetLocalTime(&now);
    wchar_t name[96] = {};
    swprintf_s(name, L"_before_switch_%04u%02u%02u_%02u%02u%02u_%03u_%lu.bak",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        now.wMilliseconds, GetCurrentProcessId());
    return name;
}

static void THDrawScoreSaveManager(const THGameSig& game, const THGameInst& instance,
    char* newSaveName, size_t newSaveNameSize, std::wstring& selectedSlotName,
    std::string& status, bool& statusIsError, std::string& statusGameId)
{
    if (statusGameId != game.idStr) {
        statusGameId = game.idStr;
        status.clear();
        statusIsError = false;
        selectedSlotName.clear();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("存档管理");

    const auto paths = THGetScoreSaveManagerPaths(game, instance);
    const auto activeSaveExists = THIsRegularFile(paths.activeSave);
    ImGui::TextWrapped("当前存档文件：%s", utf16_to_utf8(paths.activeSave.c_str()).c_str());

    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputText("新存档名称", newSaveName, newSaveNameSize);
    ImGui::SameLine();
    if (ImGui::Button("保存当前存档为命名副本")) {
        const auto safeName = THSanitizeScoreSaveName(newSaveName);
        if (!activeSaveExists) {
            status = "当前游戏存档文件不存在";
            statusIsError = true;
        } else if (safeName.empty()) {
            status = "请输入有效名称（最多 80 个字符）";
            statusIsError = true;
        } else if (!THEnsureDirectoryTree(paths.slotDirectory)) {
            status = "无法创建存档槽位目录";
            statusIsError = true;
        } else {
            const auto slotPath = THGetScoreSaveSlotPath(paths.slotDirectory, safeName + L".dat");
            if (THIsRegularFile(slotPath)) {
                status = "该名称已存在，请换名或重命名现有存档";
                statusIsError = true;
            } else if (!CopyFileW(paths.activeSave.c_str(), slotPath.c_str(), TRUE)) {
                status = std::format("保存失败（错误码 {}）", GetLastError());
                statusIsError = true;
            } else {
                selectedSlotName = safeName + L".dat";
                status = "存档副本已保存";
                statusIsError = false;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("创建新存档"))
        ImGui::OpenPopup("确认创建新存档");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("删除当前生效的存档文件；下次启动游戏时由游戏自动创建新存档");

    if (ImGui::BeginPopupModal("确认创建新存档", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("将删除当前存档文件：\n%s\n建议先关闭游戏。删除后请启动游戏，由游戏自动创建新存档。命名存档副本不会受影响。",
            utf16_to_utf8(paths.activeSave.c_str()).c_str());
        if (ImGui::Button("确认删除当前存档")) {
            DWORD error = ERROR_FILE_NOT_FOUND;
            if (!THIsRegularFile(paths.activeSave)) {
                error = ERROR_FILE_NOT_FOUND;
            } else if (DeleteFileW(paths.activeSave.c_str())) {
                status = "当前存档已删除；请启动游戏以自动创建新存档";
                statusIsError = false;
            } else {
                error = GetLastError();
                status = std::format("删除当前存档失败（错误码 {}）", error);
                statusIsError = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    auto slots = THListScoreSaveSlots(paths.slotDirectory);
    if (!selectedSlotName.empty()
        && std::find(slots.begin(), slots.end(), selectedSlotName) == slots.end())
        selectedSlotName.clear();
    const auto selectedLabel = selectedSlotName.empty() ? std::string("选择命名存档")
        : utf16_to_utf8(THScoreSaveSlotStem(selectedSlotName).c_str());
    ImGui::SetNextItemWidth(280.0f);
    if (ImGui::BeginCombo("命名存档列表", selectedLabel.c_str())) {
        for (const auto& slot : slots) {
            const auto label = utf16_to_utf8(THScoreSaveSlotStem(slot).c_str());
            const bool selected = slot == selectedSlotName;
            if (ImGui::Selectable(label.c_str(), selected))
                selectedSlotName = slot;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const bool hasSelectedSlot = !selectedSlotName.empty();
    if (!hasSelectedSlot)
        ImGui::BeginDisabled();
    if (ImGui::Button("切换到所选存档"))
        ImGui::OpenPopup("确认切换存档");
    ImGui::SameLine();
    if (ImGui::Button("重命名所选")) {
        const auto currentName = THScoreSaveSlotStem(selectedSlotName);
        strcpy_s(newSaveName, newSaveNameSize, utf16_to_utf8(currentName.c_str()).c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("删除所选"))
        ImGui::OpenPopup("确认删除存档");
    if (!hasSelectedSlot)
        ImGui::EndDisabled();

    if (hasSelectedSlot && ImGui::Button("将所选存档重命名")) {
        const auto safeName = THSanitizeScoreSaveName(newSaveName);
        if (safeName.empty()) {
            status = "请输入有效名称（最多 80 个字符）";
            statusIsError = true;
        } else {
            const auto source = THGetScoreSaveSlotPath(paths.slotDirectory, selectedSlotName);
            const auto destination = THGetScoreSaveSlotPath(paths.slotDirectory, safeName + L".dat");
            if (source == destination) {
                status = "名称没有变化";
                statusIsError = false;
            } else if (THIsRegularFile(destination)) {
                status = "该名称已存在，请使用其他名称";
                statusIsError = true;
            } else if (!MoveFileW(source.c_str(), destination.c_str())) {
                status = std::format("重命名失败（错误码 {}）", GetLastError());
                statusIsError = true;
            } else {
                selectedSlotName = safeName + L".dat";
                status = "存档已重命名";
                statusIsError = false;
            }
        }
    }

    if (ImGui::BeginPopupModal("确认切换存档", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("切换会替换当前游戏存档。现有存档会先备份到存档槽目录。建议先关闭游戏再切换。\n继续切换到“%s”？",
            selectedLabel.c_str());
        if (ImGui::Button("确认切换")) {
            const auto source = THGetScoreSaveSlotPath(paths.slotDirectory, selectedSlotName);
            const auto temporary = paths.activeSave + L".thprac_switch_tmp";
            bool canSwitch = THIsRegularFile(source) && THEnsureDirectoryTree(
                GetDirFromFullPath(paths.activeSave));
            if (canSwitch && activeSaveExists) {
                const auto backup = THGetScoreSaveSlotPath(paths.slotDirectory,
                    THMakeScoreSaveSwitchBackupName());
                canSwitch = THEnsureDirectoryTree(paths.slotDirectory)
                    && CopyFileW(paths.activeSave.c_str(), backup.c_str(), TRUE);
                if (!canSwitch) {
                    status = std::format("切换前备份失败（错误码 {}）", GetLastError());
                    statusIsError = true;
                }
            }
            if (canSwitch) {
                DeleteFileW(temporary.c_str());
                canSwitch = CopyFileW(source.c_str(), temporary.c_str(), TRUE);
                if (!canSwitch) {
                    status = std::format("读取命名存档失败（错误码 {}）", GetLastError());
                    statusIsError = true;
                } else if (!MoveFileExW(temporary.c_str(), paths.activeSave.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    const auto error = GetLastError();
                    DeleteFileW(temporary.c_str());
                    status = std::format("替换当前存档失败（错误码 {}）", error);
                    statusIsError = true;
                } else {
                    status = "存档切换完成";
                    statusIsError = false;
                }
            } else if (!statusIsError) {
                status = "所选命名存档不存在，或无法创建当前存档目录";
                statusIsError = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("确认删除存档", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("确定删除命名存档“%s”？此操作无法撤销。", selectedLabel.c_str());
        if (ImGui::Button("确认删除")) {
            const auto slotPath = THGetScoreSaveSlotPath(paths.slotDirectory, selectedSlotName);
            if (DeleteFileW(slotPath.c_str())) {
                selectedSlotName.clear();
                status = "命名存档已删除";
                statusIsError = false;
            } else {
                status = std::format("删除失败（错误码 {}）", GetLastError());
                statusIsError = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (!status.empty())
        ImGui::TextColored(statusIsError ? ImVec4(1.0f, 0.35f, 0.3f, 1.0f)
            : ImVec4(0.35f, 0.9f, 0.4f, 1.0f), "%s", status.c_str());
}

namespace {
    // ---- Hover state shared by the list rows and the cover card --------------
    // SelectableWrapped fills this in while it walks the rows; DrawGameCoverCard
    // consumes it at the end of the same frame. Only one row can be hovered, so
    // a single record is enough.
    struct GameHoverState {
        std::string id;
        std::string label;
        bool available = false;
        bool hovered = false; // a row was under the pointer this frame
        float fade = 0.0f;    // 0..1, eased, drives the card in and out

        // The card lags behind `id` so a pointer that jumps between rows does not
        // hard-cut from one cover to the next: `shown*` is what is on screen now,
        // `leaving*` dissolves out while the new cover fades in.
        std::string shownId;
        std::string shownLabel;
        bool shownAvailable = false;
        float age = 0.0f; // seconds the shown cover has been on screen
        std::string leavingId;
        std::string leavingLabel;
        bool leavingAvailable = false;
        float crossfade = 0.0f; // 1 -> 0 while the previous cover dissolves
    };
    GameHoverState g_gameHover;
    std::unordered_map<std::string, float> g_gameRowHover;

    // Eased per-row hover amount. Kept per row so a row that loses the pointer
    // shrinks back smoothly instead of snapping.
    float GameRowHoverAmount(const char* id, bool hovered)
    {
        auto& value = g_gameRowHover[id];
        const float step = ImGui::GetIO().DeltaTime * 12.0f;
        value += hovered ? step : -step;
        if (value < 0.0f)
            value = 0.0f;
        else if (value > 1.0f)
            value = 1.0f;
        return value;
    }

    uint32_t GameIdHash(const char* id)
    {
        uint32_t hash = 2166136261u;
        for (const char* p = id; p && *p; ++p)
            hash = (hash ^ (uint32_t)(unsigned char)*p) * 16777619u;
        return hash;
    }

    // Localized titles are "TH06 东方红魔乡　～ the Embodiment of Scarlet
    // Devil." -- the separator is a full-width space plus U+FF5E, not the ASCII
    // "~" the first version of this code looked for (so the romanized half was
    // never dimmed). Both spellings are accepted here.
    bool IsFullWidthTilde(const char* p)
    {
        return p && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBD && (unsigned char)p[2] == 0x9E;
    }

    bool IsFullWidthSpace(const char* p)
    {
        return p && (unsigned char)p[0] == 0xE3 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x80;
    }

    const char* FindTitleSeparator(const char* label)
    {
        if (!label)
            return nullptr;
        for (const char* p = label; *p; ++p) {
            if (*p == '~' || IsFullWidthTilde(p))
                return p;
        }
        return nullptr;
    }

    // Start of the romanized name: everything after the separator's spaces.
    const char* TitleSubtitleStart(const char* separator)
    {
        if (!separator)
            return nullptr;
        const char* p = IsFullWidthTilde(separator) ? separator + 3 : separator + 1;
        while (*p == ' ' || IsFullWidthSpace(p))
            p += (*p == ' ') ? 1 : 3;
        return p;
    }

    // Localized name without the trailing full-width space.
    std::string TitleHead(const char* label)
    {
        const char* separator = FindTitleSeparator(label);
        std::string head(label, separator ? (size_t)(separator - label) : strlen(label));
        while (!head.empty()) {
            if (head.back() == ' ') {
                head.pop_back();
            } else if (head.size() >= 3 && IsFullWidthSpace(head.c_str() + head.size() - 3)) {
                head.resize(head.size() - 3);
            } else {
                break;
            }
        }
        return head;
    }

    // Accent colour of a game's cover. The main line gets a colour that reads
    // like its box art; anything else derives a stable hue from the id so new
    // entries still look deliberate instead of falling back to grey.
    ImU32 GameAccentColor(const char* id)
    {
        static const struct {
            const char* id;
            ImU32 color;
        } kAccents[] = {
            { "th06", IM_COL32(186, 62, 58, 255) },
            { "th07", IM_COL32(126, 168, 210, 255) },
            { "th075", IM_COL32(122, 107, 181, 255) },
            { "th08", IM_COL32(86, 96, 176, 255) },
            { "th09", IM_COL32(190, 104, 150, 255) },
            { "th095", IM_COL32(150, 128, 196, 255) },
            { "th10", IM_COL32(104, 166, 92, 255) },
            { "th11", IM_COL32(198, 160, 64, 255) },
            { "th12", IM_COL32(84, 166, 160, 255) },
            { "th125", IM_COL32(126, 194, 200, 255) },
            { "th128", IM_COL32(198, 112, 168, 255) },
            { "th13", IM_COL32(176, 140, 216, 255) },
            { "th14", IM_COL32(212, 138, 62, 255) },
            { "th143", IM_COL32(196, 88, 82, 255) },
            { "th15", IM_COL32(70, 112, 190, 255) },
            { "th16", IM_COL32(128, 168, 96, 255) },
            { "th165", IM_COL32(144, 164, 84, 255) },
            { "th17", IM_COL32(158, 74, 62, 255) },
            { "th18", IM_COL32(112, 144, 216, 255) },
            { "th185", IM_COL32(112, 168, 160, 255) },
            { "th19", IM_COL32(176, 92, 138, 255) },
            { "th20", IM_COL32(198, 154, 62, 255) },
            { "alcostg", IM_COL32(162, 104, 62, 255) },
        };
        for (auto& accent : kAccents) {
            if (!strcmp(accent.id, id))
                return accent.color;
        }
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB((float)(GameIdHash(id) % 360u) / 360.0f, 0.52f, 0.66f, r, g, b);
        return IM_COL32((int)(r * 255.0f), (int)(g * 255.0f), (int)(b * 255.0f), 255);
    }

    ImU32 CoverShade(ImU32 color, float factor, int alpha)
    {
        auto channel = [&](int shift) {
            int value = (int)(((color >> shift) & 0xFF) * factor);
            return value < 0 ? 0 : (value > 255 ? 255 : value);
        };
        return IM_COL32(channel(IM_COL32_R_SHIFT), channel(IM_COL32_G_SHIFT), channel(IM_COL32_B_SHIFT), alpha);
    }

    // Procedural cover: a lit wash plus an emblem whose petal count and rotation
    // come from the game id, so every entry looks distinct without shipping any
    // artwork.
    void DrawProceduralCover(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 accent, const char* id, float u, float alpha)
    {
        auto a = [alpha](int value) { return (int)(value * alpha); };
        const float width = max.x - min.x;
        const float height = max.y - min.y;
        const ImVec2 center(min.x + width * 0.5f, min.y + height * 0.44f);
        const uint32_t seed = GameIdHash(id);

        drawList->AddRectFilledMultiColor(min, max,
            CoverShade(accent, 0.52f, a(255)), CoverShade(accent, 0.34f, a(255)),
            CoverShade(accent, 0.10f, a(255)), CoverShade(accent, 0.17f, a(255)));
        // Soft moon behind the emblem.
        drawList->AddCircleFilled(ImVec2(center.x, center.y - height * 0.05f), width * 0.44f,
            CoverShade(accent, 1.35f, a(30)), 64);

        const float radius = width * 0.27f;
        const int petals = 5 + (int)(seed % 7u);
        const float phase = (float)(seed % 360u) * 3.14159265f / 180.0f;
        for (int i = 0; i < petals; ++i) {
            const float angle = phase + (float)i * 6.2831853f / (float)petals;
            const ImVec2 petal(center.x + cosf(angle) * radius, center.y + sinf(angle) * radius);
            drawList->AddCircleFilled(petal, width * 0.045f, CoverShade(accent, 1.5f, a(150)), 20);
        }
        drawList->AddCircle(center, radius, CoverShade(accent, 1.7f, a(130)), 64, 1.5f * u);
        drawList->AddCircle(center, radius * 0.62f, CoverShade(accent, 1.7f, a(80)), 64, 1.0f * u);
        drawList->AddCircleFilled(center, radius * 0.2f, CoverShade(accent, 2.0f, a(220)), 32);

        // Bottom scrim keeps the caption area readable on bright covers.
        drawList->AddRectFilledMultiColor(ImVec2(min.x, max.y - height * 0.3f), max,
            IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, a(120)), IM_COL32(0, 0, 0, a(120)));
        drawList->AddRect(min, max, CoverShade(accent, 1.9f, a(70)));
    }

    // Draws the box art into [min,max]. A jacket is cropped to fill the slot only
    // when that costs under ~10% of it; otherwise the whole artwork is shown over
    // a mat tinted with its own average colour. (An earlier version filled the
    // bands with a stretched copy of the cover, which repeated the artwork's own
    // title and copyright line right below themselves and read as a glitch.)
    // `zoom` narrows the UV window about the centre, which is what the slow Ken
    // Burns push-in uses -- it can never expose an edge.
    void DrawCoverArt(ImDrawList* drawList, const char* gameId, ImVec2 min, ImVec2 max, float alpha, float zoom, float u)
    {
        void* texture = nullptr;
        ImVec2 pixelSize(0.0f, 0.0f);
        ImU32 matte = 0;
        if (!gameId || !*gameId || !Gui::LauncherCoverGet(gameId, &texture, pixelSize, &matte) || !texture) {
            DrawProceduralCover(drawList, min, max, GameAccentColor(gameId ? gameId : ""), gameId ? gameId : "", u, alpha);
            return;
        }

        const int tint = (int)(255.0f * alpha);
        const float artWidth = max.x - min.x;
        const float artHeight = max.y - min.y;
        const float cardAspect = artWidth / artHeight;
        const float imageAspect = pixelSize.x / pixelSize.y;
        const float aspectRatio = imageAspect / cardAspect;

        // UV window that crops the source to the card's aspect ratio.
        ImVec2 uv0(0.0f, 0.0f);
        ImVec2 uv1(1.0f, 1.0f);
        if (imageAspect > cardAspect) {
            const float visible = cardAspect / imageAspect;
            uv0.x = (1.0f - visible) * 0.5f;
            uv1.x = 1.0f - uv0.x;
        } else {
            const float visible = imageAspect / cardAspect;
            uv0.y = (1.0f - visible) * 0.5f;
            uv1.y = 1.0f - uv0.y;
        }

        // A jacket is only cropped when doing so costs less than ~10% of it;
        // anything further from the slot's aspect keeps the whole artwork and
        // sits on a mat tinted with its own average colour.
        const bool letterbox = aspectRatio > 1.10f || aspectRatio < 0.90f;
        if (!letterbox) {
            const float halfU = (uv1.x - uv0.x) * 0.5f / zoom;
            const float halfV = (uv1.y - uv0.y) * 0.5f / zoom;
            const float centerU = (uv0.x + uv1.x) * 0.5f;
            const float centerV = (uv0.y + uv1.y) * 0.5f;
            drawList->AddImage((ImTextureID)texture, min, max,
                ImVec2(centerU - halfU, centerV - halfV), ImVec2(centerU + halfU, centerV + halfV),
                IM_COL32(255, 255, 255, tint));
            return;
        }

        ImVec2 fitMin;
        ImVec2 fitMax;
        if (imageAspect > cardAspect) {
            const float height = artWidth / imageAspect;
            fitMin = ImVec2(min.x, min.y + (artHeight - height) * 0.5f);
            fitMax = ImVec2(max.x, fitMin.y + height);
        } else {
            const float width = artHeight * imageAspect;
            fitMin = ImVec2(min.x + (artWidth - width) * 0.5f, min.y);
            fitMax = ImVec2(fitMin.x + width, max.y);
        }

        if (matte) {
            // Soft mat: strongest next to the artwork, fading into the card colour
            // at the outer edge, so the bands read as a glow instead of a frame.
            const ImU32 core = (matte & ~IM_COL32_A_MASK) | ((ImU32)(int)(165.0f * alpha) << IM_COL32_A_SHIFT);
            const ImU32 edge = (matte & ~IM_COL32_A_MASK) | ((ImU32)(int)(30.0f * alpha) << IM_COL32_A_SHIFT);
            if (fitMin.y > min.y)
                drawList->AddRectFilledMultiColor(ImVec2(min.x, min.y), ImVec2(max.x, fitMin.y),
                    edge, edge, core, core);
            if (fitMax.y < max.y)
                drawList->AddRectFilledMultiColor(ImVec2(min.x, fitMax.y), ImVec2(max.x, max.y),
                    core, core, edge, edge);
            if (fitMin.x > min.x)
                drawList->AddRectFilledMultiColor(ImVec2(min.x, fitMin.y), ImVec2(fitMin.x, fitMax.y),
                    edge, core, core, edge);
            if (fitMax.x < max.x)
                drawList->AddRectFilledMultiColor(ImVec2(fitMax.x, fitMin.y), ImVec2(max.x, fitMax.y),
                    core, edge, edge, core);
        }

        drawList->AddImage((ImTextureID)texture, fitMin, fitMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
            IM_COL32(255, 255, 255, tint));
        // Hairline so the artwork does not look like it is fading into the card.
        drawList->AddRect(fitMin, fitMax, IM_COL32(255, 255, 255, (int)(26.0f * alpha)));
    }

    // Floating cover card for the hovered row. Drawn into the games panel's own
    // draw list so the panel clip rect keeps it inside the card frame; it has no
    // items, so clicks pass through to the rows underneath.
    void DrawGameCoverCard(ImVec2 panelMin, ImVec2 panelSize)
    {
        const float dt = ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime : 0.0f;
        g_gameHover.fade += g_gameHover.hovered ? dt * 9.0f : -dt * 9.0f;
        if (g_gameHover.fade < 0.0f)
            g_gameHover.fade = 0.0f;
        else if (g_gameHover.fade > 1.0f)
            g_gameHover.fade = 1.0f;

        // Swap the displayed card, keeping the old one around to dissolve out.
        if (g_gameHover.hovered && !g_gameHover.id.empty()) {
            if (g_gameHover.shownId != g_gameHover.id) {
                if (!g_gameHover.shownId.empty()) {
                    g_gameHover.leavingId = g_gameHover.shownId;
                    g_gameHover.leavingLabel = g_gameHover.shownLabel;
                    g_gameHover.leavingAvailable = g_gameHover.shownAvailable;
                    g_gameHover.crossfade = 1.0f;
                }
                g_gameHover.shownId = g_gameHover.id;
                g_gameHover.shownLabel = g_gameHover.label;
                g_gameHover.shownAvailable = g_gameHover.available;
                g_gameHover.age = 0.0f;
            } else {
                g_gameHover.age += dt;
            }
        }
        if (g_gameHover.crossfade > 0.0f) {
            g_gameHover.crossfade -= dt * 4.5f; // 0.22s, in step with the card fade-in
            if (g_gameHover.crossfade <= 0.0f) {
                g_gameHover.crossfade = 0.0f;
                g_gameHover.leavingId.clear();
                g_gameHover.leavingLabel.clear();
            }
        }
        if (g_gameHover.fade <= 0.01f || g_gameHover.shownId.empty()) {
            // Fully hidden: forget everything so the next hover starts clean.
            g_gameHover.shownId.clear();
            g_gameHover.shownLabel.clear();
            g_gameHover.leavingId.clear();
            g_gameHover.leavingLabel.clear();
            g_gameHover.crossfade = 0.0f;
            g_gameHover.age = 0.0f;
            return;
        }

        const Gui::ChromeColors& palette = Gui::GetChromeColors();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 panelMax(panelMin.x + panelSize.x, panelMin.y + panelSize.y);
        const float u = ImGui::GetFontSize() > 0.0f ? ImGui::GetFontSize() / 20.0f : 1.0f;

        const float fade = g_gameHover.fade;
        const float cardWidth = 232.0f * u;
        const float cardHeight = 374.0f * u;
        const float margin = 14.0f * u;
        // Leave the scrollbar alone; drawing over it looks like a glitch.
        const float scrollbar = ImGui::GetScrollMaxY() > 0.0f ? ImGui::GetStyle().ScrollbarSize : 0.0f;
        const ImVec2 cardMax(panelMax.x - margin - scrollbar, panelMin.y + margin + cardHeight);
        const ImVec2 cardMin(cardMax.x - cardWidth, panelMin.y + margin);
        if (cardMin.x - panelMin.x < cardWidth * 1.35f)
            return; // window too narrow to show a preview without covering the list

        // Slide in from the right while fading.
        const float slide = (1.0f - fade) * 20.0f * u;
        auto shifted = [slide](ImVec2 p) { return ImVec2(p.x + slide, p.y); };
        const ImVec2 cardMinS = shifted(cardMin);
        const ImVec2 cardMaxS = shifted(cardMax);
        auto withAlpha = [fade](ImU32 color) {
            const int alpha = (int)(((color >> IM_COL32_A_SHIFT) & 0xFF) * fade);
            return (color & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT);
        };
        auto withAlphaF = [fade](ImU32 color, float factor) {
            const int alpha = (int)(((color >> IM_COL32_A_SHIFT) & 0xFF) * fade * factor);
            return (color & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT);
        };

        drawList->AddRectFilled(cardMinS, cardMaxS, withAlpha(ImGui::GetColorU32(palette.cardBackground)), 12.0f * u);
        drawList->AddRect(cardMinS, cardMaxS, withAlpha(ImGui::GetColorU32(palette.cardBorder)), 12.0f * u, 0, 1.0f);

        const float artPad = 9.0f * u;
        const ImVec2 artMin(cardMinS.x + artPad, cardMinS.y + artPad);
        const ImVec2 artMax(cardMaxS.x - artPad, artMin.y + 262.0f * u);
        const float cross = g_gameHover.crossfade;
        const ImU32 accent = GameAccentColor(g_gameHover.shownId.c_str());

        if (cross > 0.0f && !g_gameHover.leavingId.empty()) {
            const float leavingAlpha = fade * (g_gameHover.leavingAvailable ? 1.0f : 0.45f) * cross;
            DrawCoverArt(drawList, g_gameHover.leavingId.c_str(), artMin, artMax, leavingAlpha, 1.0f, u);
        }
        // Slow push-in over the first few seconds; exponential so it settles
        // instead of stopping dead on a frame boundary.
        const float zoom = 1.0f + 0.055f * (1.0f - expf(-g_gameHover.age / 4.0f));
        const float shownAlpha = fade * (g_gameHover.shownAvailable ? 1.0f : 0.45f) * (1.0f - cross);
        DrawCoverArt(drawList, g_gameHover.shownId.c_str(), artMin, artMax, shownAlpha, zoom, u);

        // Caption: "TH06 东方红魔乡" on one line, romanized name wrapped under it.
        // It fades in with the cover, so switching rows reads as one motion.
        const float captionIn = (std::min)(g_gameHover.age / 0.18f, 1.0f);
        const float captionEase = 1.0f - (1.0f - captionIn) * (1.0f - captionIn);
        const float captionDrop = (1.0f - captionEase) * 6.0f * u;
        const char* label = g_gameHover.shownLabel.c_str();
        const char* separator = FindTitleSeparator(label);
        const std::string head = TitleHead(label);
        const ImVec4 clipRect(cardMinS.x, cardMinS.y, cardMaxS.x, cardMaxS.y);
        ImFont* font = ImGui::GetFont();
        const float pad = 12.0f * u;
        const float headSize = ImGui::GetFontSize() * 0.92f;
        const float subSize = ImGui::GetFontSize() * 0.68f;

        float textY = artMax.y + 10.0f * u + captionDrop;
        drawList->AddText(font, headSize, ImVec2(cardMinS.x + pad, textY), withAlphaF(palette.title, captionEase),
            head.c_str(), nullptr, 0.0f, &clipRect);
        textY += headSize + 6.0f * u;
        if (separator) {
            drawList->AddText(font, subSize, ImVec2(cardMinS.x + pad, textY),
                withAlphaF(palette.rowSubtitle, captionEase),
                TitleSubtitleStart(separator), nullptr, cardWidth - pad * 2.0f, &clipRect);
        }

        // Accent rule under the caption, tinted with the game's own colour.
        const float ruleY = cardMaxS.y - 12.0f * u;
        drawList->AddRectFilled(ImVec2(cardMinS.x + pad, ruleY), ImVec2(cardMaxS.x - pad, ruleY + 2.0f * u),
            withAlphaF(CoverShade(accent, 1.0f, 190), captionEase), 1.0f * u);
    }
}

class THGameGui {
    // TODO: This looks suspicious - why is it 6 when there are only 4 variants?
    static constexpr size_t GAME_SCAN_TYPE_CNT = 6;
    enum ScanOption {
        SCAN_OPT_ORIGINAL = 0,
        SCAN_OPT_MODDED = 1,
        SCAN_OPT_THCRAP = 2, // OBSOLETED
        SCAN_OPT_STEAM = 3
    };

private:
    bool CheckIfValidCatagory(const char* str)
    {
        for (auto& gameDef : gGameDefs) {
            if (!strcmp(str, gameDef.idStr)) {
                return true;
            }
        }
        return false;
    }
    bool LoadGameCfg()
    {
        bool result = true;
        int applyThpracDefault = 0;
        LauncherSettingGet("apply_thprac_default", applyThpracDefault);

        auto& cfg = LauncherCfgGet();
        if (cfg.HasMember("games") && cfg["games"].IsObject()) {
            auto& gameJson = cfg["games"];
            for (auto it = gameJson.MemberBegin(); it != gameJson.MemberEnd(); ++it) {
                if (CheckIfValidCatagory(it->name.GetString()) && it->value.IsObject()) {
                    if (it->value.HasMember("instances") && it->value["instances"].IsArray()) {
                        auto& inst = it->value["instances"];
                        for (auto gameIt = inst.Begin(); gameIt != inst.End(); ++gameIt) {
                            auto& game = *gameIt;
                            if (game.IsObject()) {
                                if (game.HasMember("path") && game["path"].IsString()) {
                                    THGameInst gameInst;
                                    gameInst.path = game["path"].GetString();
                                    if (game.HasMember("type") && game["type"].IsInt()) {
                                        gameInst.type = (THGameType)game["type"].GetInt();
                                    } else {
                                        gameInst.type = TYPE_UNCERTAIN;
                                    }
                                    if (game.HasMember("name") && game["name"].IsString()) {
                                        gameInst.name = game["name"].GetString();
                                    }

                                    if (gameInst.type == TYPE_THCRAP && IfEndWith(gameInst.path.c_str(), ".lnk")) {
                                        continue;
                                    }
                                    if (gameInst.type == TYPE_STEAM) {
                                        gameInst.path = GetCleanedPath(gameInst.path);
                                    }

                                    switch (applyThpracDefault) {
                                    case 1:
                                        gameInst.useTHPrac = IsTHPracApplicable(gameInst.type);
                                        if (!gameInst.useTHPrac && game.HasMember("apply_thprac") && game["apply_thprac"].IsBool()) {
                                            gameInst.useTHPrac = game["apply_thprac"].GetBool();
                                        }
                                        break;
                                    case 2:
                                        gameInst.useTHPrac = false;
                                        break;
                                    default:
                                        if (game.HasMember("apply_thprac") && game["apply_thprac"].IsBool()) {
                                            gameInst.useTHPrac = game["apply_thprac"].GetBool();
                                        }
                                        break;
                                    }
                                    if (game.HasMember("autoBackup") && game["autoBackup"].IsBool()) {
                                        gameInst.autoBackup = game["autoBackup"].GetBool();
                                    }

                                    if (game.HasMember("useVpatch") && game["useVpatch"].IsBool()) {
                                        gameInst.useVpatch = game["useVpatch"].GetBool();
                                    }
                                    if (game.HasMember("useOILP") && game["useOILP"].IsBool()) {
                                        gameInst.useOILP = game["useOILP"].GetBool();
                                    }
                                    mGames[it->name.GetString()].instances.push_back(gameInst);
                                } else {
                                    result = false;
                                }
                            } else {
                                game.SetObject();
                                result = false;
                            }
                        }
                        if (it->value.HasMember("default_launch") && it->value["default_launch"].IsInt()) {
                            mGames[it->name.GetString()].defaultLaunch = it->value["default_launch"].GetInt();
                        }
                    } else {
                        if (it->value.HasMember("instances")) {
                            result = false;
                            it->value["instances"].SetObject();
                        }
                    }
                } else {
                    result = false;
                    it->value.SetObject();
                }
            }
        } else {
            if (cfg.HasMember("games")) {
                result = false;
                cfg["games"].SetObject();
            }
        }

        WriteGameCfg();
        return result;
    }
    void WriteGameCfg() 
    {
        rapidjson::Document gameJson;
        auto& alloc = gameJson.GetAllocator();
        gameJson.SetObject();

        for (auto& it : mGames) {
            auto& game = it.second;
            rapidjson::Value gameTitleJson;
            gameTitleJson.SetObject();
            JsonAddMember(gameTitleJson, "default_launch", game.defaultLaunch, alloc);
            
            rapidjson::Value gameInstances;
            gameInstances.SetArray();
            for (auto& inst : game.instances) {
                rapidjson::Value gameInst;
                gameInst.SetObject();
                JsonAddMemberA(gameInst, "name", inst.name.c_str(), alloc);
                JsonAddMember(gameInst, "type", (int)inst.type, alloc);
                JsonAddMemberA(gameInst, "path", inst.path.c_str(), alloc);
                JsonAddMember(gameInst, "apply_thprac", inst.useTHPrac, alloc);
                JsonAddMember(gameInst, "autoBackup", inst.autoBackup, alloc);
                JsonAddMember(gameInst, "useVpatch", inst.useVpatch, alloc);
                JsonAddMember(gameInst, "useOILP", inst.useOILP, alloc);
                gameInstances.PushBack(gameInst, alloc);
            }
            JsonAddMemberA(gameTitleJson, "instances", gameInstances, alloc);

            JsonAddMemberA(gameJson, game.signature.idStr, gameTitleJson, alloc);
        }

        auto& cfg = LauncherCfgGet();
        if (cfg.HasMember("games")) {
            cfg.RemoveMember("games");
        }
        JsonAddMemberA(cfg, "games", gameJson, cfg.GetAllocator());
        LauncherCfgWrite();
    }
    THGameGui()
    {
        for (auto& gameDef : gGameDefs) {
            mGames[gameDef.idStr].signature = gameDef;
        }
        LoadGameCfg();
        thcrapSetup();
        mGuiUpdFunc = [&]() { GuiMain(); };
    }
    SINGLETON(THGameGui)

public:
    bool BackupScoreFile(bool auto_backup = false) {
        static char appdata[MAX_PATH];
        std::string zipfilename = "";
        bool have_backup = false;
        if (!auto_backup) {
            time_t now = time(0);
            tm* currentDate = localtime(&now);
            std::wstring filename = LauncherWndFolderSelect(nullptr);
            if (filename.empty())
                return true;
            zipfilename = utf16_to_mb(filename.c_str(), CP_ACP);
            zipfilename += std::format("\\backup{:04d}{:02d}{:02d}-{:02d}{:02d}{:02d}.zip",
                currentDate->tm_year + 1900, currentDate->tm_mon + 1, currentDate->tm_mday,
                currentDate->tm_hour, currentDate->tm_min, currentDate->tm_sec);
        } else {
            time_t now = time(0);
            tm* currentDate = localtime(&now);
            GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
            zipfilename = appdata;
            zipfilename += std::format("\\thprac\\backup-{:04d}{:02d}{:02d}-{:02d}{:02d}{:02d}.zip",
                currentDate->tm_year + 1900, currentDate->tm_mon + 1, currentDate->tm_mday,
                currentDate->tm_hour, currentDate->tm_min, currentDate->tm_sec);
        }

        struct GameBackup {
            std::vector<std::string> score_paths;
            std::string score_file_name_in_zip;
            std::string zip_folder;
        };
        std::vector<GameBackup> all_backup;

        for (auto& it : mGames) {
            auto& game = it.second;
            if (game.signature.scoreFileStr == nullptr)
                continue;
            if (strcmp(game.signature.idStr, "th06") == 0) {
                GameBackup gamebackup06;
                gamebackup06.zip_folder = "th06_spells";
                gamebackup06.score_file_name_in_zip = "score06.dat";
                for (auto& inst : game.instances) {
                    if (inst.autoBackup) {
                        have_backup = true;
                        std::string scorefilepath;
                        GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
                        scorefilepath = appdata;
                        scorefilepath += "\\ShanghaiAlice\\th06\\score06.dat";
                        gamebackup06.score_paths.push_back(scorefilepath);
                        break;
                    }
                }
                if (gamebackup06.score_paths.size() != 0)
                    all_backup.push_back(gamebackup06);
            } else if(strcmp(game.signature.idStr, "th20") == 0) {
                    GameBackup gamebackup20;
                    gamebackup20.zip_folder = "th20_pyras";
                    gamebackup20.score_file_name_in_zip = "score20pyra.dat";
                    for (auto& inst : game.instances) {
                        if (inst.autoBackup) {
                            have_backup = true;
                            std::string scorefilepath;
                            GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
                            scorefilepath = appdata;
                            scorefilepath += "\\ShanghaiAlice\\th20\\score20pyra.dat";
                            gamebackup20.score_paths.push_back(scorefilepath);
                            break;
                        }
                    }
                    if (gamebackup20.score_paths.size() != 0)
                        all_backup.push_back(gamebackup20);
                }
            GameBackup gamebackup;
            gamebackup.zip_folder = game.signature.idStr;
            gamebackup.score_file_name_in_zip = game.signature.scoreFileStr;
            for (auto& inst : game.instances) {
                if (inst.autoBackup) {
                    have_backup = true;
                    if (game.signature.appdataStr) {
                        std::string scorefilepath;
                        GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
                        scorefilepath = appdata;
                        scorefilepath += "\\ShanghaiAlice\\";
                        scorefilepath += game.signature.idStr;
                        scorefilepath += "\\";
                        scorefilepath += game.signature.scoreFileStr;
                        gamebackup.score_paths.push_back(scorefilepath);
                    } else {
                        std::string scorefilepath;
                        std::string path_MB = utf16_to_mb(utf8_to_utf16(GetDirFromFullPath(inst.path).c_str()).c_str(), CP_ACP);
                        scorefilepath = path_MB + game.signature.scoreFileStr;
                        gamebackup.score_paths.push_back(scorefilepath);
                    }
                }
            }
            if (gamebackup.score_paths.size() != 0)
                all_backup.push_back(gamebackup);
        }
        bool backup_thp_file = false;
        LauncherSettingGet("backup_thp_file", backup_thp_file);
        if (backup_thp_file)
            have_backup = true;

        if (have_backup){
            zipFile zf = zipOpen(zipfilename.c_str(), APPEND_STATUS_CREATE);

            for (auto& it : all_backup) {
                for (int i = 0; i < std::ssize(it.score_paths); i++) {
                    std::string filename_a;
                    if (it.score_paths.size() >= 2){
                        filename_a = "";
                        for (auto& c : it.score_file_name_in_zip) {
                            if (c != '.')
                                filename_a += c;
                            else
                                break;
                        }
                        filename_a += std::format("_{}.dat", i+1); 
                    } else {
                        filename_a = it.score_file_name_in_zip;
                    }
                    AddfiletoZip(zf, it.zip_folder + "\\" + filename_a, it.score_paths[i]);
                }
            }
            if (backup_thp_file) {
                std::string thppath;
                GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
                thppath = appdata;
                thppath += "\\thprac\\thprac.json";
                AddfiletoZip(zf, "thprac\\thprac.json", thppath);
                thppath = appdata;
                thppath += "\\thprac\\thpracP.json";
                AddfiletoZip(zf, "thprac\\thpracP.json", thppath);
                thppath = appdata;
                thppath += "\\thprac\\keng.dat";
                AddfiletoZip(zf, "thprac\\keng.dat", thppath);
                thppath = appdata;
                thppath += "\\thprac\\thprac_configs.db";//marketeer version
                AddfiletoZip(zf, "thprac\\thprac_configs.db", thppath);
                have_backup = true;
            }
            zipCloseFileInZip(zf);
            if (zipClose(zf, NULL) != ZIP_OK) {
                return false;
            }
            return true;
        }
        return false;
    }

    static HINSTANCE WINAPI thcrapLaunchGame(std::wstring& cfg, const char* game, const char* append = nullptr)
    {
        wchar_t thcrapDir[MAX_PATH];
        wchar_t thcrapArg[MAX_PATH];

        swprintf_s(thcrapDir, L"%s\\%s", THGameGui::singleton().mThcrapDir.c_str(), L"thcrap_loader.exe");
        swprintf_s(thcrapArg, L"\"%s\" %s%s", cfg.c_str(), utf8_to_utf16(game).c_str(), append ? utf8_to_utf16(append).c_str() : L"");
        return ShellExecuteW(nullptr, L"open", thcrapDir, thcrapArg, THGameGui::singleton().mThcrapDir.c_str(), SW_SHOW);
    }
    void thcrapPathConversion()
    {
        wchar_t currentPath[MAX_PATH];
        wchar_t cvt[MAX_PATH];
        bool isRelative = false;
        LauncherSettingGet("use_relative_path", isRelative);
        GetModuleFileNameW(GetModuleHandleW(nullptr), currentPath, MAX_PATH);

        if (isRelative) {
            if (!PathIsRelativeW(mThcrapDir.c_str())) {
                if (PathRelativePathToW(cvt, currentPath, FILE_ATTRIBUTE_DIRECTORY, mThcrapDir.c_str(), FILE_ATTRIBUTE_NORMAL)) {
                    mThcrapDir = cvt;
                    LauncherSettingSet("thcrap", utf16_to_utf8(mThcrapDir.c_str()));
                }
            }
        } else {
            if (PathIsRelativeW(mThcrapDir.c_str())) {
                if (GetFullPathNameW(mThcrapDir.c_str(), MAX_PATH, cvt, nullptr)) {
                    mThcrapDir = cvt;
                    LauncherSettingSet("thcrap", utf16_to_utf8(mThcrapDir.c_str()));
                }
            }
        }
    }
    bool thcrapLaunch()
    {
        auto configure = mThcrapDir + L"\\thcrap.exe";
        if (ShellExecuteW(nullptr, L"open", configure.c_str(), nullptr, mThcrapDir.c_str(), SW_SHOW) > (HINSTANCE)32) {
            return true;
        }
        return false;
    }
    void thcrapAdd(const char* gameId, const std::string& cfg, bool use_thprac, bool flush = false)
    {
        if (flush) {
            WriteGameCfg();
            return;
        }

        bool repeated = false;
        for (auto& game : mGames[gameId].instances) {
            if (game.path == cfg) {
                repeated = true;
                break;
            }
        }

        if (!repeated) {
            THGameInst inst;

            char nameStr[256];
            std::string subname = cfg;
            if (IfEndWith(subname.c_str(), ".js")) {
                for (auto& c : subname) {
                    c = static_cast<char>(tolower(c));
                }
                subname = subname.substr(0, subname.rfind(".js"));
            }
            sprintf_s(nameStr, "%s (%s)", gameId, subname.c_str());

            inst.name = nameStr;
            inst.path = cfg;
            inst.type = TYPE_THCRAP;
            inst.useTHPrac = use_thprac == 1 ? IsTHPracApplicable(TYPE_THCRAP) : false;
            mGames[gameId].instances.push_back(inst);
        }
    }
    void thcrapCfgGet(std::vector<std::pair<std::string, bool>>& cfgVec, std::vector<GameRoll> gameVec[4])
    {
        cfgVec.clear();
        for (auto& cfg : mThcrapCfg) {
            cfgVec.emplace_back(cfg, false);
        }
        for (int i = 0; i < 4; i++) {
            for (auto& game : gameVec[i]) {
                game.playerSelect = nullptr;
                game.selected = false;
                for (auto& gameAvail : mThcrapGames) {
                    if (gameAvail == game.name) {
                        game.playerSelect = "GAME";
                        break;
                    }
                }
            }
        }
    }
    bool thcrapTest(std::wstring& dir)
    {
        auto loaderPath = dir + L"\\thcrap_loader.exe";
        auto configurePath = dir + L"\\thcrap.exe";
        auto loaderAttr = GetFileAttributesW(loaderPath.c_str());
        auto configureAttr = GetFileAttributesW(configurePath.c_str());

        if (loaderAttr != INVALID_FILE_ATTRIBUTES && !(loaderAttr & FILE_ATTRIBUTE_DIRECTORY)) {
            if (configureAttr != INVALID_FILE_ATTRIBUTES && !(loaderAttr & FILE_ATTRIBUTE_DIRECTORY)) {
                return true;
            }
        }

        return false;
    }
    bool thcrapChkCfgFolder(std::wstring& dir)
    {
        auto jsConfig = dir + L"\\config.js";
        auto jsGames = dir + L"\\games.js";

        if (!CheckIsValidJson(jsConfig) || !CheckIsValidJson(jsGames)) {
            return false;
        }

        return true;
    }
    bool thcrapChkConfig(std::wstring& jsonPath)
    {
        void* jsBuffer;
        size_t jsSize;
        if (LoadJsonFile(jsonPath, jsBuffer, jsSize)) {
            rapidjson::Document json;
            if (!(json.Parse((const char*)jsBuffer, jsSize + 1).HasParseError()) && json.HasMember("patches") && json["patches"].IsArray() && json["patches"].Size()) {
                bool isValid = true;
                auto& patches = json["patches"];
                for (auto patchIt = patches.Begin(); patchIt != patches.End(); ++patchIt) {
                    if (!(*patchIt).IsObject()) {
                        isValid = false;
                        break;
                    }
                }
                if (isValid) {
                    free(jsBuffer);
                    return true;
                }
            }
            free(jsBuffer);
        }
        return false;
    }
    bool thcrapChkGames(std::wstring& cfgDir)
    {
        void* jsBuffer;
        size_t jsSize;
        auto gameCfgPath = cfgDir + L"\\games.js";
        if (LoadJsonFile(gameCfgPath, jsBuffer, jsSize)) {
            rapidjson::Document json;
            if (!(json.Parse((const char*)jsBuffer, jsSize + 1).HasParseError()) && json.IsObject()) {
                for (auto it = json.MemberBegin(); it != json.MemberEnd(); ++it) {
                    if (it->value.IsString() && strcmp(it->value.GetString(), "")) {
                        mThcrapGames.push_back(std::string(it->name.GetString()));
                    }
                }
            }
            free(jsBuffer);
        }
        return true;
    }
    bool thcrapSetup()
    {

        mThcrapDir = L"";
        mThcrapCfgDir = L"";
        mThcrapCfg.clear();
        mThcrapGames.clear();

        std::string thcrapDirUtf8;
        std::wstring thcrapDir;
        LauncherSettingGet("thcrap", thcrapDirUtf8);
        thcrapDir = utf8_to_utf16(thcrapDirUtf8.c_str());
        if (!thcrapTest(thcrapDir)) {
            LauncherSettingSet("thcrap", std::string(""));
            return false;
        }
        mThcrapDir = thcrapDir;
        thcrapPathConversion();

        auto cfgDir = thcrapDir + L"\\config";
        if (!thcrapChkCfgFolder(cfgDir)) {
            cfgDir = thcrapDir;
            if (!thcrapChkCfgFolder(cfgDir)) {
                return true;
            }
        }
        mThcrapCfgDir = cfgDir;

        std::wstring searchDir = cfgDir + L"\\*.js";
        WIN32_FIND_DATAW findData;
        HANDLE searchHnd = FindFirstFileW(searchDir.c_str(), &findData);
        if (searchHnd == INVALID_HANDLE_VALUE) {
            return true;
        }
        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }
            searchDir = cfgDir + L"\\" + std::wstring(findData.cFileName);

            if (thcrapChkConfig(searchDir)) {
                mThcrapCfg.push_back(utf16_to_utf8(findData.cFileName));
            }

        } while (FindNextFileW(searchHnd, &findData));
        FindClose(searchHnd);

        thcrapChkGames(cfgDir);


        return true;
    }

    void PathCoversion()
    {
        wchar_t currentPath[MAX_PATH];
        wchar_t cvt[MAX_PATH];
        bool isRelative = false;
        LauncherSettingGet("use_relative_path", isRelative);
        GetModuleFileNameW(GetModuleHandleW(nullptr), currentPath, MAX_PATH);
        

        // TODO: This variable is unused, but its assignment has a side effect.
        [[maybe_unused]] auto& gameGui = THGameGui::singleton();
        for (auto& it : mGames) {
            for (auto& gameInst : it.second.instances) {
                if (gameInst.type == TYPE_THCRAP) {
                    continue;
                }
                auto u16Path = utf8_to_utf16(gameInst.path.c_str());
                if (isRelative) {
                    if (!PathIsRelativeW(u16Path.c_str())) {
                        //if (PathRelativePathToW(cvt, currentPath, FILE_ATTRIBUTE_DIRECTORY, u16Path.c_str(), FILE_ATTRIBUTE_NORMAL)) {
                        if (PathRelativePathToW(cvt, currentPath, 0, u16Path.c_str(), FILE_ATTRIBUTE_NORMAL)) {
                            gameInst.path = utf16_to_utf8(cvt);
                        }
                    }
                } else {
                    if (PathIsRelativeW(u16Path.c_str())) {
                        if (GetFullPathNameW(u16Path.c_str(), MAX_PATH, cvt, nullptr)) {
                            gameInst.path = utf16_to_utf8(cvt);
                        }
                    }
                }
            }
        }
    }
    void ForceReload()
    {
        mGames.clear();
        for (auto& gameDef : gGameDefs) {
            mGames[gameDef.idStr].signature = gameDef;
        }
        LoadGameCfg();
        thcrapSetup();
        PathCoversion();
        WriteGameCfg();
    }
    void FinalizeGameScan()
    {
        int applyThpracDefault = 0;
        LauncherSettingGet("apply_thprac_default", applyThpracDefault);
        for (auto& scan : mGameScan) {
            for (auto& gameScanned : scan) {
                if (!gameScanned.checked) {
                    continue;
                }

                auto gameScannedPathCmp = GetUnifiedPath(gameScanned.game.path);
                bool repeated = false;
                for (auto& game : mGames[gameScanned.signature->idStr].instances) {
                    auto gameExistPathCmp = GetUnifiedPath(game.path);
                    if (gameExistPathCmp == gameScannedPathCmp) {
                        repeated = true;
                        break;
                    }
                }

                if (!repeated) {
                    gameScanned.game.useTHPrac = applyThpracDefault == 1 ? IsTHPracApplicable(gameScanned.game.type) : false;
                    mGames[gameScanned.signature->idStr].instances.push_back(gameScanned.game);
                }
            }
        }
        PathCoversion();
    }
    void ColumnsDebug(int columns)
    {
        for (int i = 0; i < columns; ++i) {
            ImGui::TextWrapped("%f", ImGui::GetColumnOffset(i));
            ImGui::NextColumn();
        }
        ImGui::Separator();
    }
    bool ColumnsScanResultPrt(std::vector<THGameScan>& games)
    {
        bool result = true;
        for (auto& game : games) {
            std::string chkBoxId = "##";
            chkBoxId += game.game.path;
            ImGui::Checkbox(chkBoxId.c_str(), &(game.checked));
            result &= game.checked;
            ImGui::NextColumn();
            ImGui::TextWrapped("%s (%s)", game.signature->idStr, S(TH_TYPE_SELECT[game.game.type]));
            ImGui::NextColumn();
            GuiColumnText(game.game.path.c_str());
            ImGui::NextColumn();
            ImGui::Separator();
        }
        return result;
    }
    void ColumnsScanResultHdr(bool& selectAll, std::vector<THGameScan>& games)
    {
        float offset[] = {
            0.0f, 2.0f, 10.0f
        };
        ImGui::Columns(3, "##@__scan_result_col", true);
        for (int i = 0; i < 3; ++i) {
            ImGui::SetColumnOffset(i, offset[i] * ImGui::GetFontSize());
        }
        ImGui::Separator();
        if (ImGui::Checkbox("##@__select_all", &selectAll)) {
            for (auto& game : games) {
                game.checked = selectAll;
            }
        }
        ImGui::NextColumn();
        ImGui::TextUnformatted(S(THPRAC_SCAN_SCAN_RESULT_C1));
        ImGui::NextColumn();
        ImGui::TextUnformatted(S(THPRAC_SCAN_SCAN_RESULT_C2));
        ImGui::NextColumn();
        ImGui::Separator();
    }
    void GuiScanCheck(const char* text, int idx, ImVec4* color = nullptr)
    {
        char childId[64];
        sprintf_s(childId, "##@__result_c%d", idx);

        if (GuiButtonAndModalYesNo(S(THPRAC_ABORT), S(THPRAC_SCAN_SCAN_ABORT_TITLE), S(THPRAC_SCAN_SCAN_ABORT_TEXT), 6.0f, S(THPRAC_OK), S(THPRAC_CANCEL))) {
            mGuiUpdFunc = [&]() { GuiMain(); };
        }
        ImGui::SameLine();
        GuiCenteredText(S(THPRAC_GAMES_SCAN_FOLDER));
        ImGui::Separator();

        if (color) {
            ImGui::PushStyleColor(ImGuiCol_Text, *color);
            ImGui::TextWrapped("%s", text);
            ImGui::PopStyleColor();

        } else {
            ImGui::TextWrapped("%s", text);
        }

        ImGui::NewLine();
        ImGui::BeginChild(childId, ImVec2(0, -2.0f * ImGui::GetFontSize()), true);
        if (mGameScanScrollReset[idx]) {
            mGameScanScrollReset[idx] = false;
            ImGui::SetScrollX(0.0f);
            ImGui::SetScrollY(0.0f);
        }
        ColumnsScanResultHdr(mGameScanSelect[idx], mGameScan[idx]);
        mGameScanSelect[idx] = ColumnsScanResultPrt(mGameScan[idx]);
        ImGui::Columns(1);
        ImGui::EndChild();

        auto buttonRes = GuiCornerButton(S(THPRAC_BACK), S(THPRAC_NEXT));
        if (buttonRes == 1) {
            GuiScanCheckMenuSwitch(idx - 1, false);
        } else if (buttonRes == 2) {
            GuiScanCheckMenuSwitch(idx + 1, true);
        }
    }
    void GuiScanCheckFinish()
    {
        GuiSetPosYRel(0.5f);
        GuiCenteredText(S(THPRAC_SCAN_COMPLETE));
        auto buttonRes = GuiCornerButton(S(THPRAC_BACK), S(THPRAC_FINISH));
        if (buttonRes == 1) {
            GuiScanCheckMenuSwitch(GAME_SCAN_TYPE_CNT - 1, false);
        } else if (buttonRes == 2) {
            FinalizeGameScan();
            WriteGameCfg();
            mGuiUpdFunc = [&]() { GuiMain(); };
        }
    }
    void GuiScanCheckUncertain()
    {
        GuiScanCheck(S(THPRAC_SCAN_CONFIRM_UNCERTAIN), 5);
    }
    void GuiScanCheckModded()
    {
        GuiScanCheck(S(THPRAC_SCAN_CONFIRM_MODDED), 4);
    }
    void GuiScanCheckSteam()
    {
        GuiScanCheck(S(THPRAC_SCAN_CONFIRM_STEAM), 3);
    }
    void GuiScanCheckThcrap()
    {
        GuiScanCheck(S(THPRAC_SCAN_CONFIRM_THCRAP), 2);
    }
    void GuiScanCheckValid()
    {
        GuiScanCheck(S(THPRAC_SCAN_CONFIRM_VALID), 1);
    }
    void GuiScanCheckMalicious()
    {
        int idx = 0;
        ImVec4 color { 255.0f, 0.0f, 0.0f, 255.0f };
        char childId[64];
        sprintf_s(childId, "##@__result_c%d", idx);

        if (GuiButtonAndModalYesNo(S(THPRAC_ABORT), S(THPRAC_SCAN_SCAN_ABORT_TITLE), S(THPRAC_SCAN_SCAN_ABORT_TEXT), 6.0f, S(THPRAC_OK), S(THPRAC_CANCEL))) {
            mGuiUpdFunc = [&]() { GuiMain(); };
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        GuiCenteredText(S(THPRAC_SCAN_MALICIOUS_TITLE));
        ImGui::PopStyleColor();
        ImGui::Separator();

        ImGui::TextWrapped("%s", S(THPRAC_SCAN_CONFIRM_MALICIOUS));

        ImGui::NewLine();
        ImGui::BeginChild(childId, ImVec2(0, -2.0f * ImGui::GetFontSize()), true);
        if (mGameScanScrollReset[idx]) {
            mGameScanScrollReset[idx] = false;
            ImGui::SetScrollX(0.0f);
            ImGui::SetScrollY(0.0f);
        }

        float offset[] = {
            0.0f, 8.0f
        };
        ImGui::Columns(2, "##@__scan_result_col", true);
        for (int i = 0; i < 2; ++i) {
            ImGui::SetColumnOffset(i, offset[i] * ImGui::GetFontSize());
        }
        ImGui::Separator();
        ImGui::TextUnformatted(S(THPRAC_SCAN_SCAN_RESULT_C1));
        ImGui::NextColumn();
        ImGui::TextUnformatted(S(THPRAC_SCAN_SCAN_RESULT_C2));
        ImGui::NextColumn();
        ImGui::Separator();
        for (auto& game : mGameScan[idx]) {
            ImGui::TextWrapped("%s (%s)", game.signature->idStr, S(TH_TYPE_SELECT[game.game.type]));
            ImGui::NextColumn();
            GuiColumnText(game.game.path.c_str());
            ImGui::NextColumn();
            ImGui::Separator();
        }
        ImGui::Columns(1);
        ImGui::EndChild();

        auto buttonRes = GuiCornerButton(S(THPRAC_BACK), S(THPRAC_NEXT));
        if (buttonRes == 1) {
            GuiScanCheckMenuSwitch(idx - 1, false);
        } else if (buttonRes == 2) {
            GuiScanCheckMenuSwitch(idx + 1, true);
        }
    }
    void GuiScanCheckMenuSwitch(int idx, bool forward = true)
    {
        int direction = forward ? 1 : -1;
        for (; idx < GAME_SCAN_TYPE_CNT && idx >= 0; idx += direction) {
            if (mGameScan[idx].size()) {
                switch (idx) {
                case 0:
                    mGuiUpdFunc = [&]() { GuiScanCheckMalicious(); };
                    return;
                case 1:
                    mGuiUpdFunc = [&]() { GuiScanCheckValid(); };
                    return;
                case 2:
                    mGuiUpdFunc = [&]() { GuiScanCheckThcrap(); };
                    return;
                case 3:
                    mGuiUpdFunc = [&]() { GuiScanCheckSteam(); };
                    return;
                case 4:
                    mGuiUpdFunc = [&]() { GuiScanCheckModded(); };
                    return;
                case 5:
                    mGuiUpdFunc = [&]() { GuiScanCheckUncertain(); };
                    return;
                }
            }
        }
        if (idx >= GAME_SCAN_TYPE_CNT) {
            mGuiUpdFunc = [&]() { GuiScanCheckFinish(); };
        } else if (idx < 0) {
            mGuiUpdFunc = [&]() { GuiScanFolder(); };
        }
    }

    static bool WINAPI CheckExeName(const std::wstring& dir, const char* idStr)
    {
        if (!strcmp(idStr, "th06")) {
            if (CheckExeName(dir, "東方紅魔郷")) {
                return true;
            }
            if (CheckExeName(dir, "搶曽峠杺嫿")) {
                return true;
            }
        }

        auto idStr16 = utf8_to_utf16(idStr);
        auto fileName = dir;

        if (fileName.rfind(L"\\") != std::string::npos) {
            fileName = fileName.substr(fileName.rfind(L"\\") + 1);
        }
        fileName = GetUnifiedPath(fileName);
        if (fileName.find(idStr16) == 0) {
            // TODO: (Reason for TODO unknown)
            if (fileName == L"th155_log.exe") {
                return false;
            }
            return true;
        }
        return false;
    }
    static THGameType CompareExeSig(ExeSig& sig1, ExeSig& sig2)
    {
        if (HashCompare(sig1.metroHash, sig2.metroHash)) {
            return TYPE_ORIGINAL;
        }
        auto oepCmp = OepCompare(sig1.oepCode, sig2.oepCode);
        if (sig1.timeStamp == sig2.timeStamp && sig1.textSize == sig2.textSize) {
            if (oepCmp) {
                return TYPE_MODDED;
            } else {
                return TYPE_MALICIOUS;
            }
        }
        return TYPE_UNCERTAIN;
    }
    static DWORD WINAPI ScanAddGame(THGameType type, std::string name, const std::string& path, THGameSig& sig)
    {
        auto& game = THGameGui::singleton();
        switch (type) {
        case THPrac::TYPE_ORIGINAL:
            if (!game.mScanOption[SCAN_OPT_ORIGINAL]) {
                return 0;
            }
            break;
        case THPrac::TYPE_MODDED:
        case THPrac::TYPE_CHINESE:
        case THPrac::TYPE_SCHINESE:
        case THPrac::TYPE_TCHINESE:
        case THPrac::TYPE_NYASAMA:
            if (!game.mScanOption[SCAN_OPT_MODDED]) {
                return 0;
            }
            break;
        case THPrac::TYPE_MALICIOUS:
        case THPrac::TYPE_UNCERTAIN:
            if (!game.mScanOption[SCAN_OPT_ORIGINAL] && !game.mScanOption[SCAN_OPT_MODDED]) {
                return 0;
            }
            break;
        default:
            break;
        }

        if (name == "" && path != "") {
            name = path;
            auto nameSubpos = name.rfind('\\');
            if (nameSubpos != std::string::npos) {
                name = name.substr(nameSubpos + 1);
            }
            name = name.substr(0, name.size() - 4);
            if (type == TYPE_STEAM) {
                name += " (Steam)";
            }
        }

        switch (type) {
        case THPrac::TYPE_MALICIOUS:
            game.mGameScan[0].emplace_back(name.c_str(), path.c_str(), type, &sig, false);
            break;
        case THPrac::TYPE_ORIGINAL:
        case THPrac::TYPE_CHINESE:
        case THPrac::TYPE_SCHINESE:
        case THPrac::TYPE_TCHINESE:
        case THPrac::TYPE_NYASAMA:
            game.mGameScan[1].emplace_back(name.c_str(), path.c_str(), type, &sig);
            break;
        case THPrac::TYPE_THCRAP:
            game.mGameScan[2].emplace_back(name.c_str(), path.c_str(), type, &sig);
            break;
        case THPrac::TYPE_STEAM:
            game.mGameScan[3].emplace_back(name.c_str(), path.c_str(), type, &sig);
            break;
        case THPrac::TYPE_MODDED:
            game.mGameScan[4].emplace_back(name.c_str(), path.c_str(), type, &sig, false);
            break;
        case THPrac::TYPE_UNCERTAIN:
            game.mGameScan[5].emplace_back(name.c_str(), path.c_str(), type, &sig, false);
            break;
        default:
            return 0;
            break;
        }
        return 1;
    }
    static DWORD WINAPI ScanSetCurrentPath(const std::wstring& dir)
    {
        auto& g = THGameGui::singleton();
        auto nextIdx = (g.mScanCurrentIdx == 0) ? 1 : 0;
        g.mScanCurrent[nextIdx] = utf16_to_utf8(dir.c_str());

        // Hopefully this is an atom operation.
        g.mScanCurrentIdx = nextIdx;

        return 0;
    }
    static DWORD WINAPI ScanSteamappPath(const std::wstring& path)
    {
        for (auto& gameDef : gGameDefs) {
            if (gameDef.steamId == nullptr) {
                continue;
            }
            std::wstring checkPath = path + L"\\appmanifest_" + gameDef.steamId + L".acf";
            if (GetFileAttributesW(checkPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                const char* steamFolder = (strcmp(gameDef.idStr, "th095") == 0) ? "th95" : gameDef.idStr; // StB: folder name diff from exe name
                ScanAddGame(TYPE_STEAM, "", utf16_to_utf8(path.c_str()) + "\\common\\" + steamFolder + '\\' + gameDef.idStr + ".exe", gameDef);
            }
        }
        return 0;
    }
    static DWORD WINAPI ScanSteam()
    {
        if (!THGameGui::singleton().mScanOption[SCAN_OPT_STEAM]) {
            return 0;
        }

        std::wstring steamPath = L"";
        DWORD dwType = REG_SZ;
        HKEY hKey = 0;
        wchar_t value[MAX_PATH];
        DWORD value_length = sizeof(value);
        if (RegOpenKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueEx(hKey, L"InstallPath", nullptr, &dwType, (LPBYTE)value, &value_length) == ERROR_SUCCESS) {
                steamPath = value;
            }
            RegCloseKey(hKey);
        }
        if (steamPath == L"") {
            if (RegOpenKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Valve\\Steam", &hKey) == ERROR_SUCCESS) {
                if (RegQueryValueEx(hKey, L"InstallPath", nullptr, &dwType, (LPBYTE)value, &value_length) == ERROR_SUCCESS) {
                    steamPath = value;
                }
                RegCloseKey(hKey);
            }
        }
        if (steamPath == L"") {
            return 0;
        }
        steamPath = GetCleanedPath(steamPath);

        ScanSteamappPath(steamPath + L"\\steamapps");
        auto libraryCfgPath = steamPath + L"\\steamapps\\libraryfolders.vdf";
        auto hCfg = CreateFileW(libraryCfgPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hCfg != INVALID_HANDLE_VALUE) {
            DWORD bytesRead;
            auto cfgSize = GetFileSize(hCfg, nullptr);
            auto cfgBuffer = malloc(cfgSize);
            memset(cfgBuffer, 0, cfgSize);
            ReadFile(hCfg, cfgBuffer, cfgSize, &bytesRead, nullptr);
            std::string cfgStr((const char*)cfgBuffer);
            std::string steamappsPath;
            steamappsPath.reserve(512);

            std::string::size_type searchPos = 0;
            for (searchPos = cfgStr.find(":\\\\");
                 searchPos != std::string::npos;
                 searchPos = cfgStr.find(":\\\\", searchPos + 1)) {
                if (isalpha(cfgStr[searchPos - 1]) && cfgStr[searchPos - 2] == '\"') {
                    auto endQuotePos = cfgStr.find("\"", searchPos);
                    if (endQuotePos != std::string::npos) {
                        steamappsPath.clear();
                        steamappsPath = cfgStr.substr(searchPos - 1, endQuotePos - searchPos + 1);
                        steamappsPath = GetCleanedPath(steamappsPath);
                        if (utf8_to_utf16(steamappsPath.c_str()) != steamPath) {
                            ScanSteamappPath(utf8_to_utf16((steamappsPath + "\\steamapps").c_str()));
                        }
                    }
                }
            }

            free(cfgBuffer);
            CloseHandle(hCfg);
        }

        return 0;
    }
    static DWORD WINAPI ScanExe(const std::wstring& dir, std::string name = "")
    {
        auto& games = THGameGui::singleton().mGames;
        auto utf8Dir = utf16_to_utf8(dir.c_str());

        MappedFile file(dir.c_str(), 1 << 23);
        if (!file.fileMapView)
            return 0;

        ExeSig exeSig;
        GetExeInfo(file.fileMapView, file.fileSize, exeSig);
        THGameSig* ifSig = nullptr;
        THGameType ifType = TYPE_UNCERTAIN;

        for (int i = 0; i < IM_ARRAYSIZE(gGameDefs); ++i) {
            auto& gameSig = gGameDefs[i];
            auto type = CompareExeSig(exeSig, gameSig.exeSig);
            if (gameSig.catagory != CAT_MAIN && gameSig.catagory != CAT_SPINOFF_STG) {
                if (CheckExeName(dir, gameSig.idStr)) {
                    ifSig = &gameSig;
                }
            }
            if (type == TYPE_MODDED) {
                ifSig = &gameSig;
                ifType = TYPE_MODDED;
                break;
            } else if (type == TYPE_MALICIOUS) {
                if (!CheckHasSteamDRM(file.fileMapView, file.fileSize)) {
                    ScanAddGame(type, name, utf8Dir, gameSig);
                }
            } else if (type != TYPE_UNCERTAIN) {
                ScanAddGame(type, name, utf8Dir, gameSig);
                return 0;
            }
        }

        for (auto& known : gKnownGames) {
            if (HashCompare(exeSig.metroHash, known.metroHash)) {
                ScanAddGame(known.type, name, utf8Dir, games[known.idStr].signature);
                return 0;
            }
        }

        if (ifSig) {
            ScanAddGame(ifType, name, utf8Dir, *ifSig);
        }

        return 0;
    }
    static DWORD WINAPI ScanFolder(const std::wstring& dir)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        // TODO: This variable is unused, but its assignment has a side effect.
        [[maybe_unused]] auto scanLnk = THGameGui::singleton().mScanOption[SCAN_OPT_THCRAP];
        std::wstring searchDir = dir + L"\\*";
        WIN32_FIND_DATAW findData;
        HANDLE searchHnd = FindFirstFileW(searchDir.c_str(), &findData);

        if (searchHnd == INVALID_HANDLE_VALUE) {
            return false;
        }

        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if ((!lstrcmpW(findData.cFileName, L".")) || (!lstrcmpW(findData.cFileName, L"..")))
                    continue;
            }

            searchDir = dir + L"\\" + std::wstring(findData.cFileName);
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                ScanFolder(searchDir);
            } else {
                ScanSetCurrentPath(searchDir);
                if (_wcsicmp(searchDir.c_str() + searchDir.size() - 4, L".exe") == 0) {
                    ScanExe(searchDir);
                }
            }

        } while (FindNextFileW(searchHnd, &findData));
        FindClose(searchHnd);
        CoUninitialize();

        return 0;
    }
    static DWORD WINAPI ScanThreadFunc([[maybe_unused]] _In_ LPVOID lpParameter)
    {
        auto& game = THGameGui::singleton();
        auto& dir = game.mScanPath;
        if (game.mScanOption[SCAN_OPT_ORIGINAL] || game.mScanOption[SCAN_OPT_MODDED] || THGameGui::singleton().mScanOption[SCAN_OPT_THCRAP]) {
            ScanFolder(dir);
        }
        ScanSteam();
        return 0;
    }
    static DWORD WINAPI RescanThreadFunc([[maybe_unused]] _In_ LPVOID lpParameter)
    {
        auto& gameGui = THGameGui::singleton();
        gameGui.thcrapSetup();
        CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);

        for (auto& it : gameGui.mGames) {
            for (auto& gameInst : it.second.instances) {
                ScanSetCurrentPath(utf8_to_utf16(gameInst.path.c_str()));
                switch (gameInst.type) {
                case THPrac::TYPE_ORIGINAL:
                case THPrac::TYPE_MODDED:
                case THPrac::TYPE_CHINESE:
                case THPrac::TYPE_SCHINESE:
                case THPrac::TYPE_TCHINESE:
                case THPrac::TYPE_MALICIOUS:
                case THPrac::TYPE_NYASAMA:
                    ScanExe(utf8_to_utf16(gameInst.path.c_str()), gameInst.name);
                    break;
                case THPrac::TYPE_STEAM: {
                    auto attr = GetFileAttributesW(utf8_to_utf16(gameInst.path.c_str()).c_str());
                    if (attr != INVALID_FILE_ATTRIBUTES && attr != FILE_ATTRIBUTE_DIRECTORY) {
                        ScanAddGame(TYPE_STEAM, gameInst.name, gameInst.path, it.second.signature);
                    }
                } break;
                case THPrac::TYPE_THCRAP:
                    for (auto& cfg : gameGui.mThcrapCfg) {
                        if (cfg == gameInst.path) {
                            ScanAddGame(gameInst.type, gameInst.name, gameInst.path, it.second.signature);
                            continue;
                        }
                    }
                    break;
                case THPrac::TYPE_UNCERTAIN:
                case THPrac::TYPE_UNKNOWN:
                    ScanAddGame(gameInst.type, gameInst.name, gameInst.path, it.second.signature);
                    break;
                default:
                    break;
                }
            }
            it.second.instances.clear();
        }

        for (auto& scan : gameGui.mGameScan) {
            for (auto& gameScanned : scan) {
                gameGui.mGames[gameScanned.signature->idStr].instances.push_back(gameScanned.game);
                gameGui.mGames[gameScanned.signature->idStr].selected = 0;
            }
        }

        gameGui.WriteGameCfg();
        CoUninitialize();
        return 0;
    }
    void ScanClear()
    {
        mScanThread.Stop();
        mScanCurrent[0] = mScanCurrent[1] = "";
        mScanCurrentIdx = 0;
        mScanPath = L"";
        mScanStatus = 0;
        mScanAnm.Reset();

        mGameScanSelect[0] = false;
        mGameScanSelect[1] = true;
        mGameScanSelect[2] = true;
        mGameScanSelect[3] = false;
        mGameScanSelect[4] = false;

        for (auto& opt : mScanOption) {
            opt = false;
        }
        for (auto& sc : mGameScanScrollReset) {
            sc = true;
        }

        for (auto& v : mGameScan) {
            v.clear();
        }

        mSteamMenuStatus = 0;
    }
    void ScanComplete()
    {
        mScanStatus = 2;
        mScanThread.Stop();
    }
    void ScanAbort()
    {
        mScanStatus = 0;
        mScanThread.Stop();
    }
    void GuiScanFolder()
    {
        if (GuiButtonAndModalYesNo(S(THPRAC_ABORT), S(THPRAC_SCAN_SCAN_ABORT_TITLE), S(THPRAC_SCAN_SCAN_ABORT_TEXT), 6.0f, S(THPRAC_OK), S(THPRAC_CANCEL))) {
            ScanAbort();
            ScanClear();
            mGuiUpdFunc = [&]() { GuiMain(); };
        }
        ImGui::SameLine();
        GuiCenteredText(S(THPRAC_GAMES_SCAN_FOLDER));
        ImGui::Separator();

        if (mScanStatus == 0) {
            ImGui::TextWrapped("%s", S(THPRAC_SCAN_INSTRUCTION));
            ImGui::NewLine();
            ImGui::TextUnformatted(S(THPRAC_SCAN_SCAN_FOR));
            ImGui::Checkbox(S(THPRAC_SCAN_ORIGINAL), &mScanOption[SCAN_OPT_ORIGINAL]);
            ImGui::Checkbox(S(THPRAC_SCAN_MODDED), &mScanOption[SCAN_OPT_MODDED]);
            ImGui::Checkbox(S(THPRAC_SCAN_STEAM), &mScanOption[SCAN_OPT_STEAM]);

            bool canProceed = !mScanOption[SCAN_OPT_ORIGINAL] && !mScanOption[SCAN_OPT_MODDED] && !mScanOption[SCAN_OPT_THCRAP] && mScanOption[SCAN_OPT_STEAM];
            if (mScanOption[SCAN_OPT_ORIGINAL] || mScanOption[SCAN_OPT_MODDED] || mScanOption[SCAN_OPT_THCRAP]) {
                ImGui::NewLine();
                if (ImGui::Button(S(THPRAC_SCAN_SELECT_FOLDER))) {
                    auto path = LauncherWndFolderSelect();
                    if (path != L"") {
                        mScanPath = path;
                    }
                }
                if (mScanPath != L"") {
                    ImGui::TextWrapped(S(THPRAC_SCAN_FOLDER_SELECTED), utf16_to_utf8(mScanPath.c_str()).c_str());
                } else {
                    ImGui::TextWrapped("%s", S(THPRAC_SCAN_FOLDER_NOT_SELECTED));
                }
                canProceed = mScanPath != L"";
            }
            if (canProceed) {
                if (GuiCornerButton(S(THPRAC_BEGIN))) {
                    mScanThread.Start();
                    mScanStatus = 1;
                }
            }
        } else if (mScanStatus == 1) {
            GuiSetPosYRel(0.5f);
            GuiSetPosXText(S(THPRAC_SCAN_SCANNING));
            ImGui::TextWrapped("%s", S(THPRAC_SCAN_SCANNING));
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(mScanAnm.Get().c_str());
            GuiCenteredText(mScanCurrent[mScanCurrentIdx].c_str());

            if (!mScanThread.IsActive()) {
                ScanComplete();
            }
        } else if (mScanStatus == 2) {
            GuiSetPosYRel(0.5f);
            GuiCenteredText(S(THPRAC_SCAN_SCAN_FINISHED));
            if (GuiCornerButton(S(THPRAC_NEXT))) {
                GuiScanCheckMenuSwitch(0);
            }
        }
    }
    bool GuiGameTypeChkBox(const char* text, int idx)
    {
        bool result = ImGui::Checkbox(text, &(mSteamGameTypeOpt[idx]));
        if (result) {
            for (auto& game : mSteamGames[idx]) {
                if (game.playerSelect) {
                    game.selected = mSteamGameTypeOpt[idx];
                }
            }
        }
        return result;
    }
    void GuiScanSteam()
    {
        if (mSteamMenuStatus == 0) {
            if (ImGui::Button(S(THPRAC_BACK))) {
                mGuiUpdFunc = [&]() { GuiMain(); };
            }
            ImGui::SameLine();
            GuiCenteredText(S(THPRAC_STEAM_MNG));
            ImGui::Separator();

            ImGui::TextWrapped("%s", S(THPRAC_STEAM_MNG_DESC));
            ImGui::NewLine();
            if (ImGui::Button(S(THPRAC_STEAM_MNG_AUTO))) {
                mScanOption[SCAN_OPT_STEAM] = true;
                mScanThread.Start();
                mScanStatus = 1;
                mGuiUpdFunc = [&]() { GuiScanFolder(); };
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(S(THPRAC_STEAM_MNG_OR));
            ImGui::SameLine();
            if (ImGui::Button(S(THPRAC_STEAM_MNG_MANUAL))) {
                for (auto& games : mSteamGames) {
                    games.clear();
                }
                for (auto& game : gGameDefs) {
                    auto gameType = (GameRollType)((int)game.catagory + 1);
                    GameRoll roll { game.idStr, THPRAC_GAMEROLL_EMPTY_SHOTTYPES, gameType, THPRAC_GAMEROLL_EMPTY_SHOTTYPES, game.steamId ? "STEAM" : nullptr, 1, false };

                    auto it = mGames.find(roll.name);
                    if (it != mGames.end()) {
                        for (auto& existingGame : it->second.instances) {
                            if (existingGame.type == TYPE_STEAM) {
                                roll.selected = true;
                                break;
                            }
                        }
                    }

                    mSteamGames[gameType].push_back(roll);
                }
                mSteamMenuStatus = 1;
            }
        } else if (mSteamMenuStatus == 1) {
            if (ImGui::Button(S(THPRAC_BACK))) {
                mSteamMenuStatus = 0;
            }
            ImGui::SameLine();
            GuiCenteredText(S(THPRAC_STEAM_MNG_MANUAL_TITLE));
            ImGui::Separator();

            ImGui::TextWrapped("%s", S(THPRAC_STEAM_MNG_MANUAL_INSTRUCTION));
            ImGui::NewLine();

            int i = 0;
            for (auto& gameType : mSteamGames) {
                if (i) {
                    bool allSelected = true;
                    bool allUnavailable = true;
                    ImGui::Columns(i == 1 ? 7 : 6, 0, false);
                    for (auto& game : gameType) {
                        if (game.playerSelect) {
                            allUnavailable = false;
                            ImGui::Checkbox(game.name, &game.selected);
                            if (!game.selected) {
                                allSelected = false;
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Checkbox(game.name, &game.selected);
                            ImGui::EndDisabled();
                        }
                        mSteamGameTypeOpt[i] = allSelected;
                        ImGui::NextColumn();
                    }
                    if (allUnavailable) {
                        mSteamGameTypeOpt[i] = false;
                    }
                    ImGui::Columns(1);
                    if (i != 3) {
                        ImGui::NewLine();
                    }
                }
                ++i;
            }
            ImGui::NewLine();
            ImGui::Indent(ImGui::GetStyle().ItemSpacing.x);
            GuiGameTypeChkBox(S(THPRAC_GAMES_MAIN_SERIES), 1);
            ImGui::SameLine();
            GuiGameTypeChkBox(S(THPRAC_GAMES_SPINOFF_STG), 2);
            ImGui::SameLine();
            GuiGameTypeChkBox(S(THPRAC_GAMES_SPINOFF_OTHERS), 3);
            ImGui::Unindent(ImGui::GetStyle().ItemSpacing.x);

            if (GuiCornerButton(S(THPRAC_APPLY))) {
                for (auto& steamGames : mSteamGames) {
                    for (auto& steamGame : steamGames) {
                        auto it = mGames.find(std::string(steamGame.name));
                        if (it != mGames.end()) {
                            bool hasSteamGame = false;
                            auto& existingGames = it->second.instances;
                            for (auto gameIt = existingGames.begin();
                                gameIt != existingGames.end();
                                ++gameIt) {
                                if ((*gameIt).type == TYPE_STEAM) {
                                    hasSteamGame = true;
                                    if (!steamGame.selected) {
                                        it->second.selected = 0;
                                        existingGames.erase(gameIt);
                                    }
                                    break;
                                }
                            }
                            if (!hasSteamGame && steamGame.selected) {
                                std::string gameName = steamGame.name;
                                gameName += " (Steam)";
                                mGameScan[3].emplace_back(gameName.c_str(), "", TYPE_STEAM, &(it->second.signature));
                            }
                        }
                    }
                }

                FinalizeGameScan();
                WriteGameCfg();
                mSteamMenuStatus = 2;
            }
        } else if (mSteamMenuStatus == 2) {
            GuiCenteredText(S(THPRAC_STEAM_MNG_MANUAL_TITLE));
            ImGui::Separator();
            GuiSetPosYRel(0.5f);
            GuiCenteredText(S(THPRAC_STEAM_MNG_MANUAL_COMPLETE));
             if (GuiCornerButton(S(THPRAC_FINISH))) {
                mGuiUpdFunc = [&]() { GuiMain(); };
            }
        }
    }

    static bool WINAPI CheckProcessOmni(PROCESSENTRY32W& proc, uintptr_t& base)
    {
        if (wcscmp(L"東方紅魔郷.exe", proc.szExeFile) && wcscmp(L"alcostg.exe", proc.szExeFile) && wcscmp(L"搶曽峠杺嫿.exe", proc.szExeFile)) {
            if (proc.szExeFile[0] != L't' || proc.szExeFile[1] != L'h')
                return false;
            if (proc.szExeFile[2] < 0x30 || proc.szExeFile[2] > 0x39)
                return false;
            if (proc.szExeFile[3] < 0x30 || proc.szExeFile[3] > 0x39)
                return false;
        }

        // Open the related process
        auto hProc = OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
            FALSE,
            proc.th32ProcessID);
        if (!hProc)
            return false;

        base = GetGameModuleBase(hProc);
        if (!base) {
            return false;
        }

        // Check THPrac signature
        DWORD sigAddr = 0;
        DWORD sigCheck = 0;
        DWORD bytesReadRPM;

        ReadProcessMemory(hProc, (void*)(base + 0x3c), &sigAddr, 4, &bytesReadRPM);
        if (bytesReadRPM != 4 || !sigAddr) {
            CloseHandle(hProc);
            return false;
        }
        ReadProcessMemory(hProc, (void*)(base + sigAddr - 4), &sigCheck, 4, &bytesReadRPM);
        if (bytesReadRPM != 4 || sigCheck) {
            CloseHandle(hProc);
            return false;
        }

        ExeSig sig;
        if (GetExeInfoEx((size_t)hProc, base, sig)) {
            for (auto& gameDef : gGameDefs) {
                if (gameDef.catagory != CAT_MAIN && gameDef.catagory != CAT_SPINOFF_STG) {
                    continue;
                }
                if (gameDef.exeSig.textSize != sig.textSize || gameDef.exeSig.timeStamp != sig.timeStamp) {
                    continue;
                }
                return true;
            }
        }
        CloseHandle(hProc);
        return false;
    }
    static DWORD WINAPI CheckProcess(DWORD process, std::wstring& exePath, uintptr_t& base)
    {
        // TODO: THPRAC SIG CHECK & EXE TIME STAMP CHECK
        int result = 0;
        // does not seem like a required check? breaks launching steam games via "Start game by default"
        // (this check passes at start of LaunchThreadFunc but not here for some reason)
        /*auto currentGame = THGameGui::singleton().mCurrentGame;
        if (!currentGame) {
            return 0;
        }*/
        auto exePathU16 = exePath;

        HANDLE hModuleSnap = INVALID_HANDLE_VALUE;
        MODULEENTRY32W me32;
        me32.dwSize = sizeof(MODULEENTRY32W);
        hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process);
        if (Module32FirstW(hModuleSnap, &me32)) {
            auto modulePath = GetUnifiedPath(std::wstring(me32.szExePath));
            if (exePathU16 == modulePath) {
                result = 1;
                base = (uintptr_t)me32.modBaseAddr;
            } else {
                while (Module32NextW(hModuleSnap, &me32)) {
                    modulePath = GetUnifiedPath(std::wstring(me32.szExePath));
                    if (exePathU16 == modulePath) {
                        result = 1;
                        base = (uintptr_t)me32.modBaseAddr;
                        break;
                    }
                }
            }
        }
        CloseHandle(hModuleSnap);

        if (result) {
            auto hProc = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
                FALSE,
                process);
            if (hProc) {
                // Check THPrac signature
                DWORD sigAddr = 0;
                DWORD sigCheck = 0;
                DWORD bytesReadRPM;
                ReadProcessMemory(hProc, (void*)(base + 0x3c), &sigAddr, 4, &bytesReadRPM);
                if (bytesReadRPM != 4 || !sigAddr) {
                    CloseHandle(hProc);
                    return 0;
                }
                ReadProcessMemory(hProc, (void*)(base + sigAddr - 4), &sigCheck, 4, &bytesReadRPM);
                if (bytesReadRPM != 4 || sigCheck) {
                    CloseHandle(hProc);
                    return 0;
                }
                CloseHandle(hProc);
            } else {
                return 0;
            }
        }

        return result;
    }
    static DWORD WINAPI WaitAndApplyTHPrac(std::wstring& exePath)
    {
        auto& gameGui = THGameGui::singleton();
        bool isOmni = exePath == L"" ? true : false;

        do {
            if (CheckIfAnyGame()) {
                // Find ongoing game
                PROCESSENTRY32W procEntry;
                MODULEENTRY32W moduleEntry;
                procEntry.dwSize = sizeof(PROCESSENTRY32W);
                moduleEntry.dwSize = sizeof(MODULEENTRY32W);
                HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (Process32FirstW(snapshot, &procEntry)) {
                    do {
                        uintptr_t base = 0;
                        bool test = isOmni ? CheckProcessOmni(procEntry, base) : CheckProcess(procEntry.th32ProcessID, exePath, base);

                        if (test) {
                            if (!base)
                                return 0;

                            auto hProc = OpenProcess(
                                PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
                                FALSE,
                                procEntry.th32ProcessID);
                            if (hProc) {
                                auto result = (ApplyGameRuntime(hProc, base));
                                CloseHandle(hProc);
                                return result ? 1 : 0;
                            }
                        }
                    } while (Process32NextW(snapshot, &procEntry));
                }
            }
            Sleep(500);
        } while (!gameGui.mLaunchAbortInd);
        return 0;
    }
    static DWORD WINAPI CheckAndLoadVPatch(HANDLE hProcess, std::wstring& dir, const wchar_t* vpatchName)
    {
        auto vpatchPath = dir + vpatchName;
        bool isRelative = false;
        LauncherSettingGet("use_relative_path", isRelative);
        if (isRelative){
            WCHAR full_path[256];
            GetFullPathName(vpatchPath.c_str(), 256, full_path, 0);
            vpatchPath = std::wstring(full_path);
        }
        if (CheckDLLFunction(vpatchPath.c_str(), "_Initialize@4")) {
            auto vpNameLength = (vpatchPath.size() + 1) * sizeof(wchar_t);
            auto pLoadLibrary = ::LoadLibraryW;
            auto remoteStr = VirtualAllocEx(hProcess, nullptr, vpNameLength, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!remoteStr)
                return 0;
            defer(VirtualFreeEx(hProcess, remoteStr, 0, MEM_RELEASE));
            WriteProcessMemory(hProcess, remoteStr, vpatchPath.data(), vpNameLength, nullptr);
            auto t = CreateRemoteThread(hProcess, nullptr, 0, (LPTHREAD_START_ROUTINE)pLoadLibrary, remoteStr, 0, nullptr);
            if (!t)
                return 0;
            WaitForSingleObject(t, INFINITE);
            return 1;
        }

        return 0;
    }
    static std::wstring WINAPI GetThcrapGamePath(const char* idStr)
    {
        void* jsBuffer;
        size_t jsSize;
        auto gameCfgPath = THGameGui::singleton().mThcrapCfgDir + L"\\games.js";
        if (LoadJsonFile(gameCfgPath, jsBuffer, jsSize)) {
            rapidjson::Document json;
            if (!(json.Parse((const char*)jsBuffer, jsSize + 1).HasParseError()) && json.IsObject()) {
                for (auto it = json.MemberBegin(); it != json.MemberEnd(); ++it) {
                    if (it->value.IsString() && !strcmp(it->name.GetString(), idStr)) {
                        free(jsBuffer);
                        return std::wstring(utf8_to_utf16(it->value.GetString()));
                    }
                }
            }
            free(jsBuffer);
        }

        return std::wstring();
    }
    static HANDLE WINAPI LaunchGameDirectly(THGame* currentGame, int instance)
    {
        bool result = true;
        if (!currentGame) {
            return 0;
        }
        auto& currentInst = currentGame->instances[instance];
        auto currentInstPath = utf8_to_utf16(currentInst.path.c_str());
        auto currentInstDir = GetDirFromFullPath(currentInstPath);

        STARTUPINFOW startup_info;
        PROCESS_INFORMATION proc_info;
        memset(&startup_info, 0, sizeof(STARTUPINFOW));
        startup_info.cb = sizeof(STARTUPINFOW);
        CreateProcessW(currentInstPath.c_str(), nullptr, nullptr, nullptr, false, CREATE_SUSPENDED, nullptr, currentInstDir.c_str(), &startup_info, &proc_info);

        uintptr_t base = GetGameModuleBase(proc_info.hProcess);

        if (currentInst.useOILP) {
            std::wstring path = currentInstDir + L"openinputlagpatch.dll";
            if (CheckDLLFunction(path.c_str(), "oilp_set_game_fps")) {
                auto oilpNameLength = (path.size() + 1) * sizeof(wchar_t);
                auto pLoadLibrary = ::LoadLibraryW;
                auto remoteStr = VirtualAllocEx(proc_info.hProcess, NULL, oilpNameLength, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (!remoteStr)
                    return 0;
                defer(VirtualFreeEx(proc_info.hProcess, remoteStr, 0, MEM_RELEASE));
                WriteProcessMemory(proc_info.hProcess, remoteStr, path.data(), oilpNameLength, NULL);
                auto t = CreateRemoteThread(proc_info.hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pLoadLibrary, remoteStr, 0, NULL);
                if (t) {
                    WaitForSingleObject(t, INFINITE);
                    CloseHandle(t);
                }
            }
        }
        else if (currentInst.useVpatch) {
            auto exeName = GetNameFromFullPath(currentInstPath);
            if (exeName == L"東方紅魔郷.exe" || exeName == L"搶曽峠杺嫿") {
                if (!CheckAndLoadVPatch(proc_info.hProcess, currentInstDir, L"vpatch_th06_unicode.dll")) {
                    CheckAndLoadVPatch(proc_info.hProcess, currentInstDir, L"vpatch_th06.dll");
                }
            } else {
                if (currentGame->signature.vPatchStr) {
                    CheckAndLoadVPatch(proc_info.hProcess, currentInstDir, currentGame->signature.vPatchStr);
                }
            }
        }
        if (currentInst.useTHPrac) {
            result = (ApplyGameRuntime(proc_info.hProcess, base));
        }

        if (!result) {
            TerminateProcess(proc_info.hProcess, ERROR_FUNCTION_FAILED);
        } else {
            ResumeThread(proc_info.hThread);
        }

        CloseHandle(proc_info.hThread);
        if (result) {
            return proc_info.hProcess;
        } else {
            CloseHandle(proc_info.hProcess);
            return 0;
        }
    }
    static DWORD WINAPI LaunchThreadFunc([[maybe_unused]] _In_ LPVOID lpParameter)
    {
        auto currentGame = THGameGui::singleton().mCurrentGame;
        if (!currentGame) {
            return 0;
        }
        auto& currentInst = currentGame->instances[currentGame->selected];
        auto currentCatagory = currentGame->signature.catagory;
        auto currentInstPath = utf8_to_utf16(currentInst.path.c_str());
        auto currentInstExePath = currentInstPath;
        auto currentInstDir = GetDirFromFullPath(currentInstPath);
        HINSTANCE executeResult = (HINSTANCE)100;
        bool useReflectiveLaunch = false;
        LauncherSettingGet("reflective_launch", useReflectiveLaunch);

        switch (currentInst.type) {
        case TYPE_ORIGINAL:
        case TYPE_MODDED:
        case TYPE_MALICIOUS:
        case TYPE_SCHINESE:
        case TYPE_TCHINESE:
            if (currentCatagory == CAT_MAIN || currentCatagory == CAT_SPINOFF_STG) {
                if (useReflectiveLaunch) {
                    if (currentInst.useOILP) {
                        executeResult = ShellExecuteW(NULL, L"open", (currentInstDir + L"oilp_loader.exe").c_str(), NULL, currentInstDir.c_str(), SW_SHOW);
                    } else if (currentGame->signature.vPatchStr && currentInst.useVpatch) {
                        executeResult = ShellExecuteW(nullptr, L"open", (currentInstDir + L"vpatch.exe").c_str(), nullptr, currentInstDir.c_str(), SW_SHOW);
                    } else {
                        executeResult = ShellExecuteW(nullptr, L"open", currentInstPath.c_str(), nullptr, currentInstDir.c_str(), SW_SHOW);
                    }
                    currentInstExePath = currentInstPath.c_str();
                    currentInstExePath = GetUnifiedPath(currentInstExePath);
                } else {
                    auto handle = LaunchGameDirectly(THGameGui::singleton().mCurrentGame, THGameGui::singleton().mCurrentGame->selected);
                    if (handle) {
                        CloseHandle(handle);
                        return 1;
                    }
                }
            } else {
                executeResult = ShellExecuteW(nullptr, L"open", currentInstPath.c_str(), nullptr, currentInstDir.c_str(), SW_SHOW);
            }
            break;
        case TYPE_NYASAMA:
            executeResult = ShellExecuteW(nullptr, L"open", currentInstPath.c_str(), nullptr, currentInstDir.c_str(), SW_SHOW);
            currentInstExePath = currentInstDir + utf8_to_utf16(currentGame->signature.idStr) + L".exe";
            currentInstExePath = GetUnifiedPath(currentInstExePath);
            break;
        case TYPE_THCRAP:
            executeResult = thcrapLaunchGame(currentInstPath, currentGame->signature.idStr);
            currentInstExePath = L"";
            break;
        case TYPE_STEAM: {
            if (!RunSteamGame(currentGame->signature))
                return 0;

            currentInstExePath = GetUnifiedPath(currentInstExePath);
            executeResult = (HINSTANCE)64;
            break;
        }
        default:
            executeResult = ShellExecuteW(nullptr, L"open", currentInstPath.c_str(), nullptr, currentInstDir.c_str(), SW_SHOW);
            break;
        }

        if (executeResult <= (HINSTANCE)32) {
            return 0;
        }

        if ((currentCatagory == CAT_MAIN || currentCatagory == CAT_SPINOFF_STG) && currentInst.useTHPrac) {
            return WaitAndApplyTHPrac(currentInstExePath);
        }

        return 1;
    }

    void GameVpatchSelection(THGameInst& currentInst)
    {
        if (mCurrentGame->signature.vPatchStr) {
            if (ImGui::Checkbox(S(THPRAC_GAMES_USE_VPATCH), &currentInst.useVpatch)) {
                if (currentInst.useVpatch) {
                    currentInst.useOILP = false;
                } 
                WriteGameCfg();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox(S(THPRAC_GAMES_USE_OILP), &currentInst.useOILP)) {
                if (currentInst.useOILP) {
                    currentInst.useVpatch = false;
                }
                WriteGameCfg();
            }
        } else {
            //currentInst.useVpatch = false;
            if (ImGui::Checkbox(S(THPRAC_GAMES_USE_OILP), &currentInst.useOILP)){
                WriteGameCfg();
            }
        }
    }

    bool IsTHPracApplicable(THGameType type)
    {
        switch (type) {
        case THPrac::TYPE_ORIGINAL:
        case THPrac::TYPE_THCRAP:
        case THPrac::TYPE_NYASAMA:
        case THPrac::TYPE_STEAM:
            return true;
        default:
            return false;
        }
    }
    bool TryLaunch(const std::wstring& path)
    {
        auto attr = GetFileAttributesW(path.c_str());
        auto dir = GetDirFromFullPath(path);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            if (ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, dir.c_str(), SW_SHOW) > (HINSTANCE)32) {
                return true;
            }
        }
        return false;
    }
    bool LaunchCustom(THGameInst& game, const char* idStr)
    {
        auto dirU16 = utf8_to_utf16(GetDirFromFullPath(game.path).c_str());

        switch (game.type) {
        case TYPE_ORIGINAL:
        case TYPE_MODDED:
        case TYPE_UNCERTAIN:
        case TYPE_MALICIOUS:
        case TYPE_UNKNOWN:
            if (TryLaunch(dirU16 + L"custom.exe"))
                return true;
            break;
        case TYPE_TCHINESE:
            if (TryLaunch(dirU16 + L"custom_cht.exe"))
                return true;
        case TYPE_CHINESE:
        case TYPE_SCHINESE:
        case TYPE_NYASAMA:
            if (TryLaunch(dirU16 + L"custom_cn.exe"))
                return true;
            if (TryLaunch(dirU16 + L"custom_c.exe"))
                return true;
            if (TryLaunch(dirU16 + L"custom_chs.exe"))
                return true;
            if (TryLaunch(dirU16 + L"custom_cht.exe"))
                return true;
            if (TryLaunch(dirU16 + L"custom.exe"))
                return true;
            break;
        case TYPE_STEAM:
            if (TryLaunch(utf8_to_utf16(GetDirFromFullPath(game.path).c_str()) + L"\\custom.exe"))
                return true;
            break;
            // TODO: Launch steam
        case TYPE_THCRAP: {
            if ((int)thcrapLaunchGame(dirU16, idStr, "_custom") > 32) {
                return true;
            }
        } break;
        default:
            break;
        }

        return false;
    }
    void GameLaunchModalOpen()
    {
        ImGui::OpenPopup("Launch##@__launch_t");
        mLaunchFailed = false;
        mLaunchThread.Start();
        mLaunchModalTimeout = 0.5f;
    }
    void GameLaunchModal()
    {
        if (GuiModalFullScreen("Launch##@__launch_t")) {
            if (mLaunchModalTimeout <= 0.0f && !mLaunchThread.IsActive()) {
                mLaunchFailed = mLaunchThread.GetExitCode() == 0;
                mLaunchThread.Stop();
                if (!mLaunchFailed) {
                    mLaunchAnm.Reset();
                    ImGui::CloseCurrentPopup();
                    int action = 0;
                    LauncherSettingGet("after_launch", action);
                    switch (action) {
                    case 1:
                        GuiLauncherMainTrigger(LAUNCHER_CLOSE);
                        break;
                    case 2:
                        break;
                    default:
                        GuiLauncherMainTrigger(LAUNCHER_MINIMIZE);
                        break;
                    }
                }
            }
            mLaunchModalTimeout -= ImGui::GetIO().DeltaTime;
            GuiSetPosYRel(0.5f);
            if (!mLaunchFailed) {
                GuiSetPosYRel(0.5f);
                GuiSetPosXText(S(THPRAC_GAMES_LAUNCHING));
                ImGui::TextWrapped("%s", S(THPRAC_GAMES_LAUNCHING));
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextUnformatted(mLaunchAnm.Get().c_str());
                if (GuiButtonTxtCentered(S(THPRAC_CANCEL), 0.8f)) {
                    mLaunchAbortInd = true;
                    mLaunchThread.Wait();
                    mLaunchThread.Stop();
                    mLaunchAbortInd = false;
                    mLaunchAnm.Reset();
                    ImGui::CloseCurrentPopup();
                }
            } else {
                GuiCenteredText(S(THPRAC_GAMES_LAUNCH_ERR));
                if (GuiButtonTxtCentered(S(THPRAC_CLOSE), 0.8f)) {
                    mLaunchThread.Stop();
                    mLaunchAnm.Reset();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }
    void GameVersionTable()
    {
        static int moveIdx = -1;

        if (!mCurrentGame) {
            return;
        }
        auto& currentGame = mCurrentGame->instances;
        auto& currentInstIdx = mCurrentGame->selected;
        auto currentCatagory = mCurrentGame->signature.catagory;
        int sourceIdx = -1;
        int destIdx = -1;

        float offset[] = {
            0.0f, 10.0f, 15.0f
        };
        ImGui::TextUnformatted(S(THPRAC_GAMES_SELECT_VER));
        ImGui::BeginChild("##@_game_version", ImVec2(0, ImGui::GetWindowHeight() * 0.35f), true);
        if (mNewGameWnd) {
            mNewGameWnd = false;
            ImGui::SetScrollX(0.0f);
            ImGui::SetScrollY(0.0f);
        }
        ImGui::Columns(3, 0, true);
        for (int i = 0; i < 3; ++i) {
            ImGui::SetColumnOffset(i, offset[i] * ImGui::GetFontSize());
        }
        ImGui::Separator();

        int i = 0;
        for (auto& game : mCurrentGame->instances) {
            std::string selId = "##__version_";
            selId += game.path;

            if (ImGui::Selectable(selId.c_str(), currentInstIdx == i, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 0), false, true)) {
                currentInstIdx = i;
            }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                moveIdx = i;
                ImGui::SetDragDropPayload("##@__dnd_gameinst", &(moveIdx), sizeof(moveIdx));
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("##@__dnd_gameinst")) {
                    sourceIdx = *(decltype(mCurrentGame->selected)*)payload->Data;
                    destIdx = i;
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::SameLine(0.0f, 0.0f);
            GuiColumnText(game.name.c_str());
            ImGui::NextColumn();
            GuiColumnText(S(TH_TYPE_SELECT[game.type]));
            ImGui::NextColumn();
            GuiColumnText(game.path.c_str());
            ImGui::NextColumn();
            ImGui::Separator();
            ++i;
        }

        ImGui::Columns(1);
        ImGui::EndChild();
        auto& currentInst = currentGame[currentInstIdx];

        if (GuiButtonModal(S(THPRAC_GAMES_RENAME), S(THPRAC_GAMES_RENAME_MODAL))) {
            strcpy_s(mRename, currentInst.name.c_str());
        }
        if (GuiModal(S(THPRAC_GAMES_RENAME_MODAL))) {
            ImGui::InputText("##@__rename_input", mRename, sizeof(mRename));
            if (GuiButtonYesNo(S(THPRAC_OK), S(THPRAC_CANCEL))) {
                currentInst.name = mRename;
                WriteGameCfg();
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        if (GuiButtonAndModalYesNo(S(THPRAC_GAMES_DELETE), S(THPRAC_GAMES_DELETE_MODAL), S(THPRAC_GAMES_DELETE_CONFIRM), 6.0f, S(THPRAC_YES), S(THPRAC_NO))) {
            currentGame.erase(currentGame.begin() + currentInstIdx);
            for (; currentInstIdx > 0 && currentInstIdx >= (int)currentGame.size(); currentInstIdx--)
                ;
            WriteGameCfg();
        }

        ImGui::SameLine();
        if (ImGui::Button(S(THPRAC_GAMES_OPEN_FOLDER))) {
            auto folderPath = GetDirFromFullPath(currentInst.path);
            if (folderPath != currentInst.path) {
                ShellExecuteW(nullptr, L"explore", utf8_to_utf16(folderPath.c_str()).c_str(), nullptr, nullptr, SW_SHOW);
            }
        }

        ImGui::SameLine();
        if (mCurrentGame->signature.appdataStr) {
            if (ImGui::Button(S(THPRAC_GAMES_OPEN_APPDATA))) {
                std::wstring dataFolder;
                wchar_t appdata[MAX_PATH];
                GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
                dataFolder = appdata;
                dataFolder += L"\\ShanghaiAlice\\";
                dataFolder += utf8_to_utf16(mCurrentGame->signature.idStr);
                ShellExecuteW(nullptr, L"explore", dataFolder.c_str(), nullptr, nullptr, SW_SHOW);
            }
        }

        ImGui::SameLine();
        auto cat = mCurrentGame->signature.catagory;
        if (cat == CAT_MAIN || cat == CAT_SPINOFF_STG) {
            if (ImGui::Button(S(THPRAC_GAMES_LAUNCH_CUSTOM))) {
                if (!LaunchCustom(currentInst, mCurrentGame->signature.idStr)) {
                    mCustomErrTxtTime = 2.0f;
                }
            }
            if (mCustomErrTxtTime > 0.0f) {
                mCustomErrTxtTime -= ImGui::GetIO().DeltaTime;
                ImGui::SameLine();
                ImGui::TextUnformatted(S(THPRAC_GAMES_LAUNCH_CUSTOM_ERR));
            }
        }

        ImGui::NewLine();
        if (currentCatagory == CAT_MAIN || currentCatagory == CAT_SPINOFF_STG) {
            switch (currentInst.type) {
            case TYPE_ORIGINAL:
                GameVpatchSelection(currentInst);
                if (ImGui::Checkbox(S(THPRAC_GAMES_APPLY_THPRAC), &currentInst.useTHPrac)) {
                    WriteGameCfg();
                }
                break;
            case TYPE_THCRAP:
            case TYPE_STEAM:
            case TYPE_NYASAMA:
                if (ImGui::Checkbox(S(THPRAC_GAMES_APPLY_THPRAC), &currentInst.useTHPrac)) {
                    WriteGameCfg();
                }
                break;
            case TYPE_MODDED:
            case TYPE_MALICIOUS:
            case TYPE_SCHINESE:
            case TYPE_TCHINESE:
                GameVpatchSelection(currentInst);
                if (ImGui::Checkbox(S(THPRAC_GAMES_FORCE_THPRAC), &currentInst.useTHPrac)) {
                    if (currentInst.useTHPrac) {
                        currentInst.useTHPrac = false;
                        ImGui::OpenPopup(S(THPRAC_GAMES_FORCE_THPRAC_MODAL));
                    } else {
                        WriteGameCfg();
                    }
                }
                if (GuiModal(S(THPRAC_GAMES_FORCE_THPRAC_MODAL))) {
                    ImGui::PushTextWrapPos(ImGui::GetIO().DisplaySize.x * 0.9f);
                    ImGui::TextWrapped("%s", S(THPRAC_GAMES_FORCE_THPRAC_CONFIRM));
                    ImGui::PopTextWrapPos();
                    if (GuiButtonYesNo(S(THPRAC_YES), S(THPRAC_NO))) {
                        currentInst.useTHPrac = true;
                        WriteGameCfg();
                    }
                    ImGui::EndPopup();
                }

                break;
            default:
                break;
            }
        }

        if (currentCatagory == CAT_MAIN || currentCatagory == CAT_SPINOFF_STG)
            ImGui::SameLine();
        bool setDefaultLaunch = mCurrentGame->defaultLaunch == currentInstIdx;
        if (ImGui::Checkbox(S(THPRAC_GAMES_DEFAULT_LAUNCH), &setDefaultLaunch)) {
            if (setDefaultLaunch) {
                mCurrentGame->defaultLaunch = currentInstIdx;
            } else  {
                mCurrentGame->defaultLaunch = -1;
            }
            WriteGameCfg();
        }
        ImGui::SameLine();
        HelpMarker(S(THPRAC_GAMES_DEFAULT_LAUNCH_DESC));

        if (sourceIdx != -1 && destIdx != -1) {
            auto gameTmp = currentGame[sourceIdx];
            if (destIdx > sourceIdx) {
                currentGame.insert(currentGame.begin() + destIdx + 1, gameTmp);
                currentGame.erase(currentGame.begin() + sourceIdx);
            } else if (destIdx < sourceIdx) {
                currentGame.insert(currentGame.begin() + destIdx, gameTmp);
                currentGame.erase(currentGame.begin() + sourceIdx + 1);
            }
            if (mCurrentGame->defaultLaunch == currentInstIdx) {
                mCurrentGame->defaultLaunch = destIdx;
            }
            currentInstIdx = destIdx;
            WriteGameCfg();
        }

        if (mCurrentGame->signature.scoreFileStr != nullptr)
        {
            if (strncmp(mCurrentGame->signature.scoreFileStr, "scoreth", 7) == 0) {
                THDrawScoreSaveManager(mCurrentGame->signature, currentInst,
                    mSaveSlotName, sizeof(mSaveSlotName), mSelectedSaveSlotName,
                mSaveManagerStatus, mSaveManagerStatusIsError, mSaveManagerStatusGameId);
            }
        }

    }
    void GuiGame()
    {
        if (ImGui::Button(S(THPRAC_BACK))) {
            mGuiUpdFunc = [&]() { GuiMain(); };
        }
        ImGui::SameLine();
        GuiCenteredText(S(mCurrentGame->signature.refStr));
        ImGui::Separator();

        // The instance editor grew past the window height once the theme got
        // roomier padding, and the old absolutely-positioned launch button then
        // painted over the save manager. Give the body its own scroll area and
        // keep the primary action in the layout flow below it.
        const float launchHeight = ImGui::GetFrameHeight() * 1.3f;
        ImGui::BeginChild("##game_body", ImVec2(0.0f, -(launchHeight + ImGui::GetStyle().ItemSpacing.y)),
            false, ImGuiWindowFlags_NoBackground);

        if (!mCurrentGame || !mCurrentGame->instances.size()) {
            GuiSetPosYRel(0.5f);
            GuiCenteredText(S(THPRAC_GAMES_MISSING));
        } else {
            GameVersionTable();
        }

        ImGui::EndChild();

        if (ImGui::Button(S(THPRAC_GAMES_LAUNCH_GAME),
                ImVec2(ImGui::GetContentRegionAvail().x, launchHeight)))
            GameLaunchModalOpen();
        GameLaunchModal();
    }

    bool GuiMainCtxMenu(int type, int* result = nullptr)
    {
        if (type == 0) {
            if (!ImGui::BeginPopupContextWindow()) {
                return false;
            }
        } else {
            if (!ImGui::BeginPopupContextItem()) {
                return false;
            }
        }
        if (type == 1) {
            if (ImGui::Selectable(S(THPRAC_GAMES_DETAILS_PAGE))) {
                if (result) {
                    *result = 1;
                }
            }
            ImGui::Separator();
        }

        if (ImGui::Selectable(S(THPRAC_GAMES_SCAN_FOLDER))) {
            ScanClear();
            mGuiUpdFunc = [&]() { GuiScanFolder(); };
        }
        if (ImGui::Selectable(S(THPRAC_STEAM_MNG_BUTTON))) {
            ScanClear();
            mGuiUpdFunc = [&]() { GuiScanSteam(); };
        }
        ImGui::Separator();
        if (ImGui::Selectable(S(THPRAC_GAMES_RESCAN))) {
            mNeedRescan = true;
        }
        ImGui::EndPopup();

        return true;
    }
    void SwitchToGame(const char* idStr)
    {
        for (auto& it : mGames) {
            if (!strcmp(it.second.signature.idStr, idStr)) {
                mCurrentGame = &it.second;
                mNewGameWnd = true;
                mGuiUpdFunc = [&]() { GuiGame(); };
                return;
            }
        }

        mGuiUpdFunc = [&]() { GuiMain(); };
    }
    bool SelectableWrapped(const char* label, const char* gameId, bool disabled, bool selected, bool configured)
    {
        const float u = ImGui::GetFontSize() > 0.0f ? ImGui::GetFontSize() / 20.0f : 1.0f;
        // Localized titles read "TH06 东方红魔乡　～ the Embodiment of Scarlet
        // Devil."; the latin part is redrawn dimmed so the row keeps one
        // visual anchor while still showing the romanized name.
        const char* separator = FindTitleSeparator(label);
        const std::string head = TitleHead(label);

        // The row paints its own text now (it grows under the pointer), so the
        // selectable is only a hit box with a stable id.
        ImGui::PushID(label);
        const float rowHeight = ImGui::GetTextLineHeight() * 2.0f + 14.0f * u;
        ImGui::PushStyleColor(ImGuiCol_Header, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, 0);
        const bool result = ImGui::Selectable("##row", selected,
            disabled ? ImGuiSelectableFlags_Disabled : 0, ImVec2(0.0f, rowHeight), false);
        ImGui::PopStyleColor(3);
        ImGui::PopID();

        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        const bool hovered = ImGui::IsItemHovered(disabled ? ImGuiHoveredFlags_AllowWhenDisabled : 0);
        const float hover = GameRowHoverAmount(gameId, hovered);
        if (hovered) {
            g_gameHover.id = gameId;
            g_gameHover.label = label;
            g_gameHover.available = !disabled;
            g_gameHover.hovered = true;
        }

        const Gui::ChromeColors& palette = Gui::GetChromeColors();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Row plate: grows a couple of pixels under the pointer, so the row reads
        // as lifting off the page rather than just changing colour.
        // The threshold keeps the tail of the fade from painting a ghost plate on
        // the neighbouring row while the pointer sits on a row boundary.
        const float grow = 2.0f * u * hover;
        if (selected || hover > 0.06f) {
            const ImU32 base = selected ? palette.rowRail : palette.rowRailHovered;
            const int alpha = selected ? (int)(32.0f + 20.0f * hover) : (int)(26.0f * hover);
            drawList->AddRectFilled(
                ImVec2(rowMin.x + 1.0f * u, rowMin.y + 2.0f * u - grow),
                ImVec2(rowMax.x - 1.0f * u, rowMax.y - 2.0f * u + grow),
                (base & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT), 6.0f * u);
        }

        // Text is drawn at a slightly larger size on hover; both parts share the
        // row's vertical centre so the line stays put while it grows.
        const float headSize = ImGui::GetFontSize();
        const float headX = rowMin.x + 10.0f * u;
        const float centerY = (rowMin.y + rowMax.y) * 0.5f;
        ImFont* font = ImGui::GetFont();
        const ImU32 headColor = ImGui::GetColorU32(disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text);
        // Clip each row before the status rail; long localized names must not
        // paint over neighbouring controls or outside their hit target.
        drawList->PushClipRect(rowMin, ImVec2(rowMax.x - 20.0f * u, rowMax.y), true);
        drawList->AddText(font, headSize, ImVec2(headX, rowMin.y + 5.0f * u), headColor, head.c_str());

        if (separator) {
            const float subSize = ImGui::GetFontSize() * 0.82f;
            drawList->AddText(font, subSize,
                ImVec2(headX, rowMin.y + headSize + 9.0f * u),
                palette.rowSubtitle, TitleSubtitleStart(separator));
        }
        drawList->PopClipRect();

        // Accent rail on the active row: bright when selected, faint on hover.
        if (selected || hover > 0.06f) {
            drawList->AddRectFilled(
                ImVec2(rowMin.x + 2.0f * u, rowMin.y + 3.0f * u - grow),
                ImVec2(rowMin.x + 5.0f * u, rowMax.y - 3.0f * u + grow),
                selected ? palette.rowRail : palette.rowRailHovered, 1.5f * u);
        }

        // Availability dot, only while the row is in focus so the list stays
        // quiet at rest.
        {
            const ImVec2 dot(rowMax.x - 10.0f * u, centerY);
            if (configured) {
                drawList->AddCircleFilled(dot, 6.0f * u, (palette.dotAvailable & ~IM_COL32_A_MASK) | (40u << IM_COL32_A_SHIFT), 20);
                drawList->AddCircleFilled(dot, 3.0f * u, palette.dotAvailable, 16);
            } else {
                drawList->AddCircleFilled(dot, 3.0f * u, palette.dotUnavailable, 16);
            }
        }

        return result;
    }
    // Category heading: accent bar, tinted caption, and a hairline fading out
    // to the right edge of the panel.
    void SectionHeader(const char* title)
    {
        const float u = ImGui::GetFontSize() > 0.0f ? ImGui::GetFontSize() / 20.0f : 1.0f;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float availW = ImGui::GetContentRegionAvail().x;
        const float lineH = ImGui::GetTextLineHeight();
        const float barW = 3.0f * u;

        ImGui::SetCursorScreenPos(ImVec2(pos.x + barW + 9.0f * u, pos.y));
        const Gui::ChromeColors& palette = Gui::GetChromeColors();
        drawList->AddRectFilled(ImVec2(pos.x, pos.y + lineH * 0.16f),
            ImVec2(pos.x + barW, pos.y + lineH * 0.84f), palette.sectionRail, barW * 0.5f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette.sectionTitle), "%s", title);

        const float captionEnd = pos.x + barW + 9.0f * u + ImGui::CalcTextSize(title).x + 12.0f * u;
        const float lineEnd = pos.x + availW;
        const float lineY = pos.y + lineH * 0.5f;
        if (lineEnd > captionEnd) {
            drawList->AddRectFilledMultiColor(ImVec2(captionEnd, lineY), ImVec2(lineEnd, lineY + u),
                palette.sectionLine, palette.sectionLine & ~IM_COL32_A_MASK,
                palette.sectionLine & ~IM_COL32_A_MASK, palette.sectionLine);
        }

        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + lineH + 7.0f * u));
    }
    void GameTable(const char* title, THGameCatagory catagory)
    {
        int columns = 1;
        SectionHeader(title);
        ImGui::Columns(columns);
        for (size_t i = 0; i < elementsof(gGameDefs); i++) {
            // In case I need multiple gameDefs for the same game cause I, let's say
            // want to support multiple versions of the same game
            //
            // Yes I'm comparing string pointers and not actual strings. This is OK here.
            auto& gameRef = gGameDefs[i];
            if (i != 0 && gameRef.idStr == gGameDefs[i - 1].idStr) {
                continue;
            }
            if (gameRef.catagory == catagory) {
                auto& game = mGames[gameRef.idStr];

                auto disabled = !game.instances.size();
                const std::string searchable = std::string(game.signature.idStr) + " " + S(game.signature.refStr);
                if ((mConfiguredOnly && disabled) || !mLibraryFilter.PassFilter(searchable.c_str()))
                    continue;
                ++mVisibleGames;
                if (SelectableWrapped(S(game.signature.refStr), game.signature.idStr, disabled, mSelectedGameTmp == &game, !disabled)) {
                    bool autoDefaultLaunch = false;
                    LauncherSettingGet("auto_default_launch", autoDefaultLaunch);
                    auto autoLaunch = game.defaultLaunch;
                    if (autoLaunch < 0 && autoDefaultLaunch) {
                        autoLaunch = 0;
                    }
                    if (autoLaunch >= 0 && autoLaunch < (int)game.instances.size()) {
                        mCurrentGame = &game;
                        mCurrentGame->selected = autoLaunch;
                        GameLaunchModalOpen();
                    } else {
                        mCurrentGame = &game;
                        mNewGameWnd = true;
                        mGuiUpdFunc = [&]() { GuiGame(); };
                    }
                }
                if (!disabled) {
                    int result = 0;
                    if (GuiMainCtxMenu(1, &result)) {
                        mSelectedGameTmp = &game;
                    } else if (mSelectedGameTmp == &game) {
                        mSelectedGameTmp = nullptr;
                    }
                    if (result) {
                        mCurrentGame = &game;
                        mNewGameWnd = true;
                        mGuiUpdFunc = [&]() { GuiGame(); };
                    }
                } else if (game.signature.steamId && ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
                    if (ImGui::Selectable(S(THPRAC_GOTO_STEAM_PAGE))) {
                        std::wstring steamURL { L"https://store.steampowered.com/app/" };
                        steamURL += game.signature.steamId;
                        ShellExecuteW(nullptr, L"open", steamURL.c_str(), nullptr, nullptr, SW_SHOW);
                    }
                    ImGui::EndPopup();
                }
                ImGui::NextColumn();
            }
        }
        ImGui::Columns(1);
    }
    void GuiMain()
    {
        mCurrentGame = nullptr;
        mCustomErrTxtTime = 0.0f;
        mNeedRescan = false;
        // Rows re-assert this while the pointer is over one; whatever is left at
        // the end of the frame decides whether the cover card stays up.
        g_gameHover.hovered = false;

        GuiMainCtxMenu(0);

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##library_search",
                Gui::DesignText("搜索游戏名称或编号，例如 TH15", "Search title or ID, e.g. TH15", "ゲーム名や番号で検索（例: TH15）"),
                mLibraryFilter.InputBuf, sizeof(mLibraryFilter.InputBuf)))
            mLibraryFilter.Build();
        ImGui::Checkbox(Gui::DesignText("仅显示已配置", "Configured only", "登録済みのみ"), &mConfiguredOnly);
        if (ImGui::Button(S(THPRAC_GAMES_SCAN_FOLDER))) {
            ScanClear();
            mGuiUpdFunc = [&]() { GuiScanFolder(); };
        }
        const float steamWidth = ImGui::CalcTextSize(S(THPRAC_STEAM_MNG_BUTTON)).x + ImGui::GetStyle().FramePadding.x * 2;
        if (ImGui::GetContentRegionAvail().x > ImGui::GetItemRectSize().x + steamWidth + ImGui::GetStyle().ItemSpacing.x)
            ImGui::SameLine();
        if (ImGui::Button(S(THPRAC_STEAM_MNG_BUTTON))) {
            ScanClear();
            mGuiUpdFunc = [&]() { GuiScanSteam(); };
        }
        ImGui::TextDisabled("%s", Gui::DesignText("点击游戏进入 · 右键查看更多操作", "Select a game · Right-click for more actions", "ゲームを選択 · 右クリックでその他の操作"));
        ImGui::Separator();
        // Search and management stay visible while the library scrolls.
        const ImVec2 libraryOrigin = ImGui::GetCursorScreenPos();
        const ImVec2 librarySize = ImGui::GetContentRegionAvail();
        const float coverScale = ImGui::GetFontSize() / 20.0f;
        const bool showCover = librarySize.x >= 750.0f * coverScale && librarySize.y >= 400.0f * coverScale;
        ImGui::BeginChild("##library_list", ImVec2(showCover ? librarySize.x - 266.0f * coverScale : 0, 0), false);
        mVisibleGames = 0;
        GameTable(S(THPRAC_GAMES_MAIN_SERIES), CAT_MAIN);
        ImGui::NewLine();
        GameTable(S(THPRAC_GAMES_SPINOFF_STG), CAT_SPINOFF_STG);
        ImGui::NewLine();
        GameTable(S(THPRAC_GAMES_SPINOFF_OTHERS), CAT_SPINOFF_OTHERS);
        if (!mVisibleGames) {
            ImGui::TextWrapped("%s", Gui::DesignText("没有匹配的游戏。请修改搜索词、取消筛选，或扫描游戏文件夹。", "No matching games. Change the search, disable the filter, or scan a game folder.", "該当するゲームがありません。検索やフィルターを変更するか、フォルダーをスキャンしてください。"));
        }
        ImGui::EndChild();

        if (showCover)
            DrawGameCoverCard(libraryOrigin, librarySize);

        if (mNeedRescan) {
            ImGui::OpenPopup("rescan##@__rescan");
            mRescanModalTimeout = 1.0f;
            ScanClear();
            for (auto& opt : mScanOption) {
                opt = true;
            }
            mRescanThread.Start();
        }
        if (GuiModalFullScreen("rescan##@__rescan")) {
            if (mRescanModalTimeout <= 0.0f && !mRescanThread.IsActive()) {
                mRescanThread.Stop();
                ScanClear();
                ImGui::CloseCurrentPopup();
            }
            mRescanModalTimeout -= ImGui::GetIO().DeltaTime;
            GuiSetPosYRel(0.5f);
            GuiSetPosXText(S(THPRAC_GAMES_RESCANNING));
            ImGui::TextWrapped("%s", S(THPRAC_GAMES_RESCANNING));
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(mScanAnm.Get().c_str());
            GuiCenteredText(mScanCurrent[mScanCurrentIdx].c_str());
            ImGui::EndPopup();
        }

        GameLaunchModal();
    }
    void GuiUpdate()
    {
        mGuiUpdFunc();
    }
    std::vector<std::wstring> GetAllGamePaths()
    {
        std::vector<std::wstring> paths; 
        for (auto& ig : mGames){
            for (auto& inst : ig.second.instances)
            {
                paths.push_back(utf8_to_utf16(inst.path.c_str()));
            }
        }
        return paths;
    }
private:
    ImGuiTextFilter mLibraryFilter;
    bool mConfiguredOnly = false;
    int mVisibleGames = 0;
    std::function<void(void)> mGuiUpdFunc = []() {};
    std::unordered_map<std::string, THGame> mGames;
    THGame* mCurrentGame = nullptr;
    bool mNewGameWnd = false;
    char mRename[256];
    char mSaveSlotName[128] = {};
    std::wstring mSelectedSaveSlotName;
    std::string mSaveManagerStatus;
    std::string mSaveManagerStatusGameId;
    bool mSaveManagerStatusIsError = false;
    float mCustomErrTxtTime = 0.0f;
    GuiThread mLaunchThread { THGameGui::LaunchThreadFunc };
    float mLaunchModalTimeout = 0;
    bool mLaunchFailed = false;
    bool mLaunchAbortInd = false;
    GuiWaitingAnm mLaunchAnm;
    bool mNeedRescan = false;
    THGame* mSelectedGameTmp = nullptr;

    std::wstring mThcrapDir = L"";
    std::wstring mThcrapCfgDir = L"";
    std::vector<std::string> mThcrapCfg;
    std::vector<std::string> mThcrapGames;

    // Scanning
    float mRescanModalTimeout = 0;
    GuiThread mScanThread { THGameGui::ScanThreadFunc };
    GuiThread mRescanThread { THGameGui::RescanThreadFunc };
    int mScanStatus = 0;
    bool mScanOption[4];
    GuiWaitingAnm mScanAnm;
    std::wstring mScanPath = L"";
    std::vector<THGameScan> mGameScan[GAME_SCAN_TYPE_CNT];
    bool mGameScanScrollReset[GAME_SCAN_TYPE_CNT] { true, true, true, true, true, true };
    bool mGameScanSelect[GAME_SCAN_TYPE_CNT] { false, true, true, true, false, false };
    std::string mScanCurrent[2];
    int mScanCurrentIdx = 0;
    int mSteamMenuStatus = 0;
    std::vector<GameRoll> mSteamGames[4];
    bool mSteamGameTypeOpt[4];
};

bool LauncherGamesGuiUpd()
{
    THGameGui::singleton().GuiUpdate();
    return true;
}

bool LauncherGamesDestroy(){
    bool autobackup = false;
    if(LauncherSettingGet("auto_backup_score_file", autobackup) &&autobackup)
        THGameGui::singleton().BackupScoreFile(true);
    return true;
}

void LauncherGamesGuiSwitch(const char* idStr)
{
    THGameGui::singleton().SwitchToGame(idStr);
}

void LauncherGamesForceReload()
{
    THGameGui::singleton().ForceReload();
}

bool LauncherGamesThcrapTest(std::wstring& dir)
{
    return THGameGui::singleton().thcrapTest(dir);
}

bool LauncherGamesThcrapSetup()
{
    return THGameGui::singleton().thcrapSetup();
}

void LauncherGamesThcrapCfgGet(std::vector<std::pair<std::string, bool>>& cfgVec, std::vector<GameRoll> gameVec[4])
{
    THGameGui::singleton().thcrapCfgGet(cfgVec, gameVec);
}

void LauncherGamesThcrapAdd(const char* gameId, const std::string& cfg, bool use_thprac, bool flush)
{
    THGameGui::singleton().thcrapAdd(gameId, cfg, use_thprac, flush);
}

bool LauncherGamesThcrapLaunch()
{
    return THGameGui::singleton().thcrapLaunch();
}
std::vector<std::wstring> GetAllGamePaths()
{
    return THGameGui::singleton().GetAllGamePaths();
}
}
