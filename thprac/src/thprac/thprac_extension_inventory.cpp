#include "thprac_extension_inventory.h"
#include <Windows.h>
#include <algorithm>
#include <cwchar>
#include <fstream>
#include <rapidjson/document.h>
#include <string>

namespace THPrac {
namespace {
namespace fs = std::filesystem;
constexpr wchar_t disabledSuffix[] = L".disabled";

struct ScopeDirectory { ExtensionScope scope; const wchar_t* name; };
constexpr ScopeDirectory scopeDirectories[] {
    {ExtensionScope::Launcher, L"launcher"},
    {ExtensionScope::Game, L"game"},
    {ExtensionScope::Shared, L"shared"}
};

const wchar_t* ScopeName(ExtensionScope scope)
{
    for (const auto& entry : scopeDirectories)
        if (entry.scope == scope) return entry.name;
    return L"";
}

bool EndsWith(const std::wstring& name, const wchar_t* suffix)
{
    const auto length = wcslen(suffix);
    return name.size() >= length && _wcsicmp(name.c_str() + name.size() - length, suffix) == 0;
}

std::wstring ActiveName(const InstalledExtension& extension)
{
    auto name = extension.path.filename().wstring();
    if (extension.renamed) name.resize(name.size() - (std::size(disabledSuffix) - 1));
    return name;
}

std::wstring SettingKey(const InstalledExtension& extension)
{
    std::wstring key = L"file.";
    for (const auto ch : ActiveName(extension)) {
        if (ch == L'=') key += L"%3D";
        else if (ch == L'%') key += L"%25";
        else if (ch > 0x7e) {
            // Profile files can be ANSI. Keep keys ASCII even for Unicode DLL names.
            constexpr wchar_t hex[] = L"0123456789ABCDEF";
            key += L"%u";
            for (int shift = 12; shift >= 0; shift -= 4) key += hex[(ch >> shift) & 0xf];
        } else key += ch;
    }
    return key;
}

bool ReadEnabled(const fs::path& settings, const InstalledExtension& extension)
{
    return GetPrivateProfileIntW(ScopeName(extension.scope), SettingKey(extension).c_str(), 1, settings.c_str()) != 0;
}
}

std::filesystem::path ExtensionDirectory(void* module)
{
    wchar_t path[32768];
    const auto length = GetModuleFileNameW(static_cast<HMODULE>(module), path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return {};
    return fs::path(path).parent_path() / L"plugins";
}

std::filesystem::path GlobalExtensionDirectory()
{
    wchar_t appData[32768];
    const auto length = GetEnvironmentVariableW(L"APPDATA", appData, static_cast<DWORD>(std::size(appData)));
    if (!length || length >= std::size(appData)) return {};
    const fs::path directory(appData);
    return directory.is_absolute() ? directory / L"thprac" / L"plugins" : fs::path {};
}

static void ScanExtensionDirectory(const fs::path& directory, bool global, std::vector<InstalledExtension>& extensions)
{
    if (directory.empty()) return;
    const auto settings = directory / L"enabled.ini";
    const auto first = extensions.size();
    for (const auto& scope : scopeDirectories) {
        std::error_code error;
        for (fs::directory_iterator it(directory / scope.name, error), end; !error && it != end; it.increment(error)) {
            std::error_code fileError;
            if (!it->is_regular_file(fileError)) continue;
            const auto name = it->path().filename().wstring();
            const bool renamed = EndsWith(name, L".thp.dll.disabled");
            if (!renamed && !EndsWith(name, L".thp.dll")) continue;
            InstalledExtension extension {it->path(), scope.scope, renamed, false, global};
            extension.enabled = !renamed && ReadEnabled(settings, extension);
            extensions.push_back(std::move(extension));
        }
    }
    std::sort(extensions.begin() + first, extensions.end(), [](const auto& left, const auto& right) {
        return left.path < right.path;
    });
}

std::vector<InstalledExtension> InstalledExtensions(const fs::path& local, const fs::path& global)
{
    std::vector<InstalledExtension> extensions;
    ScanExtensionDirectory(local, false, extensions);
    std::error_code error;
    if (!global.empty() && (local.empty() || !fs::equivalent(local, global, error)))
        ScanExtensionDirectory(global, true, extensions);
    return extensions;
}

ExtensionMetadata ReadExtensionMetadata(const InstalledExtension& extension, int locale)
{
    const auto path = extension.path.parent_path() / (ActiveName(extension) + L".json");
    std::error_code error;
    const auto length = fs::file_size(path, error);
    if (error || !length || length > 8192) return {};

    std::ifstream input(path, std::ios::binary);
    std::string source(static_cast<size_t>(length), '\0');
    if (!input.read(source.data(), static_cast<std::streamsize>(source.size()))) return {};

    rapidjson::Document document;
    if (document.Parse<rapidjson::kParseValidateEncodingFlag>(source.data(), source.size()).HasParseError()
        || !document.IsObject()) return {};
    const auto settings = document.FindMember("settings");
    ExtensionMetadata metadata;
    metadata.hasSettings = settings != document.MemberEnd() && settings->value.IsTrue();
    const auto* language = locale == 0 ? "zh" : locale == 2 ? "ja" : "en";
    for (const auto* key : {language, "en", "zh", "ja"}) {
        const auto field = document.FindMember(key);
        if (field != document.MemberEnd() && field->value.IsString() && field->value.GetStringLength()) {
            metadata.description.assign(field->value.GetString(), field->value.GetStringLength());
            break;
        }
    }
    return metadata;
}

bool SetExtensionEnabled(const InstalledExtension& extension, bool enabled)
{
    const auto directory = extension.path.parent_path().parent_path();
    std::error_code error;
    if (directory.empty() || !fs::is_regular_file(extension.path, error)) return false;
    const auto settings = directory / L"enabled.ini";
    const auto section = ScopeName(extension.scope);
    const auto key = SettingKey(extension);
    if (extension.renamed) {
        if (!enabled) return true;
        const auto target = extension.path.parent_path() / ActiveName(extension);
        // Never replace an installed DLL. If writing the preference fails,
        // the legacy filename stays disabled.
        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
        const auto wasEnabled = ReadEnabled(settings, extension);
        if (!WritePrivateProfileStringW(section, key.c_str(), L"1", settings.c_str())) return false;
        if (MoveFileW(extension.path.c_str(), target.c_str())) return true;
        if (!wasEnabled) WritePrivateProfileStringW(section, key.c_str(), L"0", settings.c_str());
        return false;
    }
    return WritePrivateProfileStringW(section, key.c_str(), enabled ? L"1" : L"0", settings.c_str()) != FALSE;
}
}
