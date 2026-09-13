/**
 * @file file.hpp
 * @brief Reading and writing a whole file, and the one place that knows how a path crosses the OS.
 *
 * ## Why this is a module and not two functions somewhere else
 *
 * Two callers need it and they sit in different layers: the document persistence contract
 * (`core/authoring/persist`) and every format plugin that writes a file (`plugins/formats/...`). Leaving
 * the helpers in either one makes the other depend on a module whose subject is something else -- a CSV
 * trace exporter including the *document* contract to open a path, or the export contract pretending its
 * "writes no file itself" sentence is still true.
 *
 * ## The platform knowledge this exists to hold in one place
 *
 * A path in this platform is UTF-8, because it comes from a Qt file dialog in a program that speaks
 * UTF-8, and because a student's directory name is likely not ASCII. A path passed to a C runtime open
 * call as a narrow string is interpreted in the process code page instead, which on a Chinese Windows
 * means the file that gets opened is not the one the user chose -- or, more often, that nothing opens and
 * the user is told their file does not exist. Writing that conversion once, correctly, is cheaper than
 * writing it correctly twice.
 *
 * ## Why `absent` is its own answer
 *
 * "There is no such file" and "the file is there and I may not read it" are different problems: the first
 * is a path the user has to correct, the second is a permission or a lock they have to resolve. A single
 * failure code sends them to the wrong one roughly half the time.
 *
 * @ownership   pure (no state)
 * @thread      any (each call opens its own stream)
 * @pre         none
 * @post        none
 * @invariant   No function here interprets the bytes it moves
 * @errors      Reports through `FileOutcome` rather than throwing
 * @frozen      no
 * @tests       file.round_trip_writes_and_reads_bytes
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace qp::runtime {

/**
 * @brief Converts a UTF-8 path into the filesystem's own path type -- the one place this conversion happens.
 *
 * Public because a caller that needs a `std::filesystem::path` rather than a whole-file read has no business
 * writing the conversion again, and every repetition of it is a chance to reach for `path{std::string}`, which
 * on Windows reads the bytes in the process code page. The plugin host scans a directory and needs exactly
 * this; before it was exported, the choice was between duplicating the `char8_t` dance and depending on a
 * module whose subject is whole-file I/O for a path helper.
 *
 * @param utf8_path The path as UTF-8 bytes.
 *
 * @ownership   owns the returned path
 * @thread      any
 * @pre         none
 * @post        The result names the same file the UTF-8 path does, on every platform
 * @invariant   An empty input yields an empty path rather than a path to the current directory
 * @errors      May allocate
 * @complexity  O(length)
 * @nondet      none
 * @frozen      no
 * @tests       file.a_path_that_is_not_ascii_still_names_the_file
 */
[[nodiscard]] std::filesystem::path to_path(const std::string& utf8_path);

/**
 * @brief Why a whole-file read or write did not happen.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   One code per distinct reason; `ok` is zero
 * @errors      noexcept
 * @frozen      yes -- the tags are frozen; the set may gain a reason, because the alternative is a
 *              caller that cannot tell one failure from another
 * @tests       file.names_are_stable
 */
enum class FileOutcome : std::uint8_t {
    ok = 0,
    /// The path names nothing.
    absent = 1,
    /// The path names something that could not be read: a directory, a locked file, no permission.
    unreadable = 2,
    /// The destination could not be written: a missing directory, no permission, no space.
    unwritable = 3,
};

/**
 * @brief Stable short name of an outcome, for a message or a log line.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Non-null for every enumerator
 * @invariant   Distinct codes have distinct names
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       file.names_are_stable
 */
[[nodiscard]] constexpr const char* to_string(FileOutcome outcome) noexcept {
    switch (outcome) {
        case FileOutcome::ok: return "ok";
        case FileOutcome::absent: return "absent";
        case FileOutcome::unreadable: return "unreadable";
        case FileOutcome::unwritable: return "unwritable";
    }
    return "unknown";
}

/**
 * @brief Reads a whole file into `bytes`.
 *
 * The file is read as **binary**: nothing here knows whether the bytes are text, and a caller that
 * assumed line endings were translated would corrupt a format that stores anything else.
 *
 * @param utf8_path The path, UTF-8 encoded.
 * @param bytes     Cleared at entry; holds the whole file on success.
 *
 * @ownership   owns `bytes`'s content
 * @thread      any
 * @pre         none
 * @post        On `ok`, `bytes` holds every byte of the file; otherwise `bytes` is empty
 * @invariant   A failure never leaves a partial read behind: a caller that received half a file would
 *              have no way to tell that from a file that is genuinely short, and the shorter answer is
 *              the one that looks like data
 * @errors      noexcept; `absent` when the path names nothing, `unreadable` otherwise
 * @complexity  O(size)
 * @nondet      only through the filesystem
 * @frozen      no
 * @tests       file.round_trip_writes_and_reads_bytes,
 *              file.absent_is_told_apart_from_unreadable,
 *              file.a_path_that_is_not_ascii_still_names_the_file
 */
[[nodiscard]] FileOutcome read_whole_file(const std::string& utf8_path, std::string& bytes) noexcept;

/**
 * @brief Writes `bytes` to a file, replacing whatever was there.
 *
 * Does **not** create parent directories. A save dialog hands over a path in a directory that exists, and
 * a helper that quietly created the rest of a mistyped path would make a user's typo look like a
 * successful save.
 *
 * @param utf8_path The path, UTF-8 encoded.
 * @param bytes     The bytes to write. Any bytes: this layer does not interpret them.
 *
 * @ownership   observes `bytes`
 * @thread      any
 * @pre         none
 * @post        On `ok`, the file exists and holds exactly `bytes`; otherwise the caller is told
 * @invariant   Never reports success for a write that failed part-way -- a truncated file whose author
 *              believes it is complete is the worst outcome this function can produce
 * @errors      noexcept; `unwritable`
 * @complexity  O(size)
 * @nondet      only through the filesystem
 * @frozen      no
 * @tests       file.round_trip_writes_and_reads_bytes, file.unwritable_destination_is_reported,
 *              file.a_path_that_is_not_ascii_still_names_the_file
 */
[[nodiscard]] FileOutcome write_whole_file(const std::string& utf8_path,
                                           std::string_view bytes) noexcept;

}  // namespace qp::runtime
