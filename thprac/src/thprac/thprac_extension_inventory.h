#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace THPrac {
enum class ExtensionScope { Launcher, Game, Shared };

struct InstalledExtension {
    std::filesystem::path path;
    ExtensionScope scope;
    bool renamed;
    bool enabled;
    bool global = false;
};

std::filesystem::path ExtensionDirectory(void* module);
std::filesystem::path GlobalExtensionDirectory();
std::vector<InstalledExtension> InstalledExtensions(const std::filesystem::path& local,
    const std::filesystem::path& global = {});
struct ExtensionMetadata {
    std::string description;
    bool hasSettings = false;
};
ExtensionMetadata ReadExtensionMetadata(const InstalledExtension& extension, int locale);
bool SetExtensionEnabled(const InstalledExtension& extension, bool enabled);
}
