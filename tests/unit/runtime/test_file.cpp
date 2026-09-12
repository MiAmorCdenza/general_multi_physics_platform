/**
 * @file test_file.cpp
 * @brief Tests for whole-file I/O.
 *
 * ## What is worth testing here
 *
 * The module exists because two of the three things it does are easy to get subtly wrong:
 *
 *   - **a failed read must not leave a partial buffer** -- a caller that received half a file has no way
 *     to tell that from a file that is genuinely short, and the shorter answer looks like data;
 *   - **`absent` and `unreadable` are different answers** -- one is a path to correct, the other is a
 *     permission or a lock to resolve, and a single code sends the user to the wrong one;
 *   - **the bytes are not interpreted** -- a text-mode read would translate line endings and silently
 *     rewrite a format that stores anything but text.
 *
 * The path conversion (UTF-8 to whatever the platform's open call wants) is exercised implicitly: every
 * case here goes through it, and one of them uses a directory whose name is not ASCII, which is the case
 * that a narrow-string implementation fails on Windows and passes on Linux -- the kind of defect that
 * only ever appears on a user's machine.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/runtime/file/file.hpp>

#include <support/temp_dir.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

using namespace qp::runtime;
using qp::test::TempDir;

namespace {


/// @brief A directory name that is not ASCII, written as bytes so the test says which bytes it means.
///
/// U+7528 U+6237 U+5B9E U+9A8C -- the kind of directory a student on this platform has, and the case a
/// narrow-string path implementation gets wrong on Windows while looking correct on POSIX.
constexpr const char* kNonAsciiLeaf = "\xE7\x94\xA8\xE6\x88\xB7\xE5\xAE\x9E\xE9\xAA\x8C";

}  // namespace

TEST_CASE("file.names_are_stable", "[file]") {
    // The names go into log lines and into the sentences a user reads, so they are part of the contract
    // rather than an implementation detail.
    STATIC_REQUIRE(std::string_view(to_string(FileOutcome::ok)) == "ok");
    STATIC_REQUIRE(std::string_view(to_string(FileOutcome::absent)) == "absent");
    STATIC_REQUIRE(std::string_view(to_string(FileOutcome::unreadable)) == "unreadable");
    STATIC_REQUIRE(std::string_view(to_string(FileOutcome::unwritable)) == "unwritable");
    // ok is the zero value, so a default-constructed outcome means "proceed".
    STATIC_REQUIRE(static_cast<std::uint8_t>(FileOutcome::ok) == 0);
    REQUIRE(std::string{to_string(static_cast<FileOutcome>(200))} == "unknown");
}

TEST_CASE("file.round_trip_writes_and_reads_bytes", "[file]") {
    // Bytes in, the same bytes out -- including the ones a text-mode stream would have rewritten: a lone
    // CR, a NUL, and a byte above ASCII that is not valid UTF-8. A format is entitled to store all three.
    const TempDir dir{"roundtrip"};
    const std::string path = dir.path("bytes.bin");

    const std::string original = std::string{"a\r\nb\rc\x00d"} + "\xE4\xBD\x8D" + "\xFF";
    REQUIRE(write_whole_file(path, original) == FileOutcome::ok);

    std::string read_back;
    REQUIRE(read_whole_file(path, read_back) == FileOutcome::ok);
    REQUIRE(read_back == original);
    REQUIRE(read_back.size() == original.size());

    // An empty file is a file: `ok` with nothing in it, which is a different answer from a failure.
    REQUIRE(write_whole_file(path, "") == FileOutcome::ok);
    read_back = "stale contents from before";
    REQUIRE(read_whole_file(path, read_back) == FileOutcome::ok);
    REQUIRE(read_back.empty());

    // Writing again replaces rather than appends.
    REQUIRE(write_whole_file(path, "short") == FileOutcome::ok);
    REQUIRE(write_whole_file(path, "much longer than the previous contents") == FileOutcome::ok);
    REQUIRE(read_whole_file(path, read_back) == FileOutcome::ok);
    REQUIRE(read_back == "much longer than the previous contents");
}

TEST_CASE("file.a_path_that_is_not_ascii_still_names_the_file", "[file]") {
    // The reason the module exists. On Windows a narrow path is interpreted in the process code page, so this
    // case would either write into a differently-named file or fail outright -- and on POSIX it would pass
    // either way, which is why it is written down as a case rather than trusted. Asserted on this machine's
    // MSVC run first: the case failed, and the failure was in the *fixture*, which built the path with
    // `path::string()` and so handed the module a mojibake path. The shared `TempDir` speaks UTF-8 now.
    //
    // The non-ASCII part is the **file name**, appended as the bytes it already is; the directory keeps an
    // ASCII tag, because creating it goes through a narrow string where no such care is possible.
    const TempDir dir{"nonascii"};
    const std::string path = dir.path(std::string{kNonAsciiLeaf} + ".bin");

    REQUIRE(write_whole_file(path, "payload") == FileOutcome::ok);
    std::string read_back;
    REQUIRE(read_whole_file(path, read_back) == FileOutcome::ok);
    REQUIRE(read_back == "payload");

    // A directory whose name is not ASCII as well, so both halves of a path are exercised.
    const std::string nested = dir.dir() + "/" + kNonAsciiLeaf + "/data.bin";
    REQUIRE(write_whole_file(nested, "x") == FileOutcome::unwritable);   // the directory does not exist
}

TEST_CASE("file.absent_is_told_apart_from_unreadable", "[file]") {
    // Two problems, two fixes. A single code would send the user to the wrong one about half the time.
    const TempDir dir{"distinguish"};

    std::string bytes = "stale";
    REQUIRE(read_whole_file(dir.path("nothing_here.bin"), bytes) == FileOutcome::absent);
    // A failed read leaves the buffer empty rather than holding whatever the caller passed in.
    REQUIRE(bytes.empty());

    // A directory exists and is not a file: reading it is not "absent", and reporting it as absent would
    // send the user looking for a file they are standing in.
    REQUIRE(read_whole_file(dir.dir(), bytes) == FileOutcome::unreadable);
    REQUIRE(bytes.empty());

    // An empty path names nothing at all.
    REQUIRE(read_whole_file("", bytes) == FileOutcome::absent);
    REQUIRE(write_whole_file("", "x") == FileOutcome::unwritable);
}

TEST_CASE("file.unwritable_destination_is_reported", "[file]") {
    // Every one of these paths is one a file dialog can produce, and none of them may be reported as a
    // successful save: a user who believes they have their data is worse off than one who was refused.
    const TempDir dir{"unwritable"};

    REQUIRE(write_whole_file(dir.path("no_such_directory") + "/file.bin", "x") ==
            FileOutcome::unwritable);
    // The directory itself, rather than a file inside it.
    REQUIRE(write_whole_file(dir.dir(), "x") == FileOutcome::unwritable);
    // And nothing was created by the attempts: a failed write leaves no file claiming to hold the data.
    std::string bytes;
    REQUIRE(read_whole_file(dir.path("no_such_directory"), bytes) == FileOutcome::absent);
}
