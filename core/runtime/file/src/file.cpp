/**
 * @file file.cpp
 * @brief Whole-file I/O, and the UTF-8 path conversion that has to be right on Windows.
 */
#include <qp/runtime/file/file.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <system_error>

namespace qp::runtime {

std::filesystem::path to_path(const std::string& utf8_path) {
    if (utf8_path.empty()) return std::filesystem::path{};
    // The conversion is explicit because a path here is UTF-8 and the underlying open call is not: on
    // Windows a narrow path is interpreted in the process code page, so a directory name that is not ASCII
    // (the common case for this platform's users) would open something else, or nothing. Constructing the
    // path from the UTF-8 bytes -- `std::filesystem::path` from a `char8_t` string, which is the C++20 form
    // rather than the deprecated `u8path` -- is what makes the stream open the file the user chose.
#if defined(__cpp_char8_t)
    const auto* begin = reinterpret_cast<const char8_t*>(utf8_path.data());
    return std::filesystem::path{std::u8string{begin, begin + utf8_path.size()}};
#else
    return std::filesystem::path{utf8_path};
#endif
}

FileOutcome read_whole_file(const std::string& utf8_path, std::string& bytes) noexcept {
    // Cleared first, and that is the contract rather than tidiness: a caller that handed in a buffer
    // holding something else would otherwise see a failed read as "the file holds what I passed in".
    bytes.clear();
    if (utf8_path.empty()) return FileOutcome::absent;

    std::error_code ec;
    const std::filesystem::path path = to_path(utf8_path);
    if (!std::filesystem::exists(path, ec)) return FileOutcome::absent;
    // A directory exists and cannot be read as a file; saying `absent` would send the user looking for a
    // missing file they are standing in.
    if (std::filesystem::is_directory(path, ec)) return FileOutcome::unreadable;

    std::ifstream file{path, std::ios::binary};
    if (!file) return FileOutcome::unreadable;

    // Read the whole thing in one call rather than line by line: a line-based read would translate line
    // endings and silently rewrite the bytes a format stored.
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) return FileOutcome::unreadable;
    file.seekg(0, std::ios::beg);

    std::string content;
    content.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        file.read(content.data(), static_cast<std::streamsize>(size));
        if (!file) return FileOutcome::unreadable;
    }
    bytes = std::move(content);
    return FileOutcome::ok;
}

FileOutcome write_whole_file(const std::string& utf8_path, std::string_view bytes) noexcept {
    if (utf8_path.empty()) return FileOutcome::unwritable;

    std::ofstream file{to_path(utf8_path), std::ios::binary | std::ios::trunc};
    if (!file) return FileOutcome::unwritable;
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    // Checked after the flush, so a disk that filled up part-way is reported instead of being passed off
    // as a complete file.
    if (!file) return FileOutcome::unwritable;
    return FileOutcome::ok;
}

}  // namespace qp::runtime
