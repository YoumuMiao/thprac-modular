#include <config.h>
#include <thtk/thtk.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <string>

namespace {
namespace fs = std::filesystem;
using Stream = std::unique_ptr<thtk_io_t, decltype(&thtk_io_close)>;
using Archive = std::unique_ptr<thdat_t, decltype(&thdat_free)>;

int Repack(unsigned version, const wchar_t* source, const wchar_t* target, const wchar_t* replacements,
    thtk_error_t** error)
{
    Stream input(thtk_io_open_file_w(source, L"rb", error), thtk_io_close);
    if (!input) return 1;
    Archive original(thdat_open(version, input.get(), error), thdat_free);
    if (!original) return 1;
    const auto count = thdat_entry_count(original.get(), error);
    if (count <= 0) return 1;

    Stream output(thtk_io_open_file_w(target, L"w+b", error), thtk_io_close);
    if (!output) return 1;
    Archive rebuilt(thdat_create(version, output.get(), count, error), thdat_free);
    if (!rebuilt) return 1;
    for (int index = 0; index < count; ++index) {
        const auto* name = thdat_entry_get_name(original.get(), index, error);
        if (!name || !thdat_entry_set_name(rebuilt.get(), index, name, error)) return 1;
    }
    if (!thdat_init(rebuilt.get(), error)) return 1;

    // Use entry indices so same-named resources retain their order and payloads.
    for (int index = 0; index < count; ++index) {
        const auto* name = thdat_entry_get_name(original.get(), index, error);
        if (!name) return 1;
        Stream data(thtk_io_open_growing_memory(error), thtk_io_close);
        if (!data) return 1;
        if (thdat_entry_read_data(original.get(), index, data.get(), error) < 0) {
            std::fprintf(stderr, "Cannot read archive entry %d (%s).\n", index, name);
            return 1;
        }
        // Some archive readers return a success flag, so measure the decoded stream.
        const auto size = thtk_io_seek(data.get(), 0, SEEK_CUR, error);
        if (size < 0 || thtk_io_seek(data.get(), 0, SEEK_SET, error) != 0) {
            std::fprintf(stderr, "Cannot read archive entry %d (%s).\n", index, name);
            return 1;
        }
        const std::string key(name);
        const bool eligible = key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.")
            == std::string::npos && key != "." && key != "..";
        if (eligible) {
            const auto replacement = fs::path(replacements) / key;
            if (fs::is_regular_file(replacement)) {
                if (fs::file_size(replacement) != static_cast<unsigned long long>(size)) {
                    std::fprintf(stderr, "Unexpected metadata length for archive entry %d (%s).\n", index, name);
                    return 1;
                }
                data.reset(thtk_io_open_file_w(replacement.c_str(), L"rb", error));
                if (!data) return 1;
            }
        }
        if (thdat_entry_write_data(rebuilt.get(), index, data.get(), size, error) < 0) {
            std::fprintf(stderr, "Cannot write archive entry %d (%s).\n", index, name);
            return 1;
        }
    }
    return thdat_close(rebuilt.get(), error) ? 0 : 1;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5) return 2;
    wchar_t* end = nullptr;
    const auto version = std::wcstoul(argv[1], &end, 10);
    if (!version || *end || fs::exists(argv[3])) return 2;
    thtk_error_t* error = nullptr;
    int result;
    try {
        result = Repack(version, argv[2], argv[3], argv[4], &error);
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "%s\n", failure.what());
        result = 1;
    }
    if (result) std::fprintf(stderr, "%s\n", error ? thtk_error_message(error) : "Could not rebuild the metadata archive.");
    if (error) thtk_error_free(&error);
    return result;
}
