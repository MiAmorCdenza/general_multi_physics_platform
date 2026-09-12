/**
 * @file temp_dir.hpp
 * @brief A self-removing temporary directory whose paths are **UTF-8**, for the tests that touch files.
 *
 * ## Why the path strings are not simply `path::string()`
 *
 * Every module that opens a file in this platform takes a **UTF-8** path, because that is what a Qt file
 * dialog hands over and because a student's directory name is likely not ASCII. `std::filesystem::path:
 * :string()` converts to the platform's *narrow* encoding instead -- on Windows that is the process code
 * page -- so a fixture that builds a path with `string()` and hands it to the module is testing a
 * conversion the module would never receive. On an ASCII path the two agree, which is why such a fixture
 * passes until someone's user name or directory is not ASCII, and then fails in a way that looks like the
 * module is broken.
 *
 * So the fixture speaks the module's encoding: paths go out as UTF-8 bytes (`u8string()`, whose C++20 form
 * is a `char8_t` string and therefore needs the byte copy below), and only the ASCII leaf used to *create*
 * the directory is passed as a narrow string, where every encoding agrees.
 *
 * @ownership   owns (the directory)
 * @thread      main
 * @pre         `leaf` is ASCII
 * @post        The destructor removes the directory and everything in it
 * @invariant   `path()` returns a UTF-8 byte string
 * @errors      A directory that cannot be created leaves the fixture with paths that fail, which is what
 *              the calling test then reports
 * @frozen      no
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace qp::test {

/// @brief A `std::filesystem::path` as the UTF-8 bytes this platform's file APIs take.
[[nodiscard]] inline std::string utf8_path(const std::filesystem::path& path) {
#if defined(__cpp_char8_t)
    const std::u8string bytes = path.u8string();
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
#else
    return path.u8string();
#endif
}

/**
 * @brief A directory under the system temporary directory that removes itself.
 *
 * The name is unique per instance, from a steady clock, so two cases in one process cannot collide.
 */
class TempDir final {
public:
    /// @param leaf An **ASCII** tag naming the case, so a failure is traceable to a directory. Defaulted
    ///             rather than required: a case that does not care should not have to invent a label, and
    ///             uniqueness comes from the clock stamp rather than from the tag.
    explicit TempDir(const std::string& leaf = "tmp") {
        const auto stamp = std::to_string(static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        dir_ = std::filesystem::temp_directory_path() / ("qp_test_" + stamp + "_" + leaf);
        std::error_code ignored;
        std::filesystem::create_directories(dir_, ignored);
    }

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(dir_, ignored);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /// @brief A file inside the directory, as a UTF-8 path.
    ///
    /// `name` is appended as the bytes it already is, so a test can use a non-ASCII file name -- which is
    /// the case that matters -- without this helper inventing an encoding for it.
    [[nodiscard]] std::string path(const std::string& name) const {
        return utf8_path(dir_) + "/" + name;
    }

    /// @brief The directory itself, as a UTF-8 path.
    [[nodiscard]] std::string dir() const { return utf8_path(dir_); }

private:
    std::filesystem::path dir_{};
};

}  // namespace qp::test
