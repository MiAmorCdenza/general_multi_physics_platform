/**
 * @file icons.hpp
 * @brief Pixel-art icons, authored here as bitmaps rather than shipped as image files.
 *
 * ## Why the icons are source code
 *
 * Charter C5 asks for a projection-first UI and the goal for this phase allows pixel art packaged as icon
 * images. They are authored as **bitmaps in this file** rather than as PNGs under `resources/`, for three
 * reasons that are all about this project's rules rather than about taste:
 *
 *   - a `.png` is binary, so it cannot be reviewed in a diff, cannot be regenerated from the repository, and is
 *     the one kind of asset nothing here can assert anything about;
 *   - the layer gate forbids Qt inside `core/` and the palette work already established that a view's colours
 *     belong in a form a test can read -- an icon is eight rows of eight palette indices, which is data;
 *   - there is no image library in the build, and adding one to draw a floppy disk would be a much larger
 *     dependency than the feature.
 *
 * ## What an icon is made of
 *
 * `IconBitmap` is a glyph plus the **role** each of its ink colours plays. The roles are palette fields, not
 * literals, so an icon cannot reintroduce the hard-coded colours that `theme.hpp` exists to remove: recolor the
 * palette and every icon follows, and `theme.icons_use_palette_ink` checks that they do.
 *
 * `.` is transparent. Every other character indexes `IconBitmap::ink`.
 *
 * @ownership   pure (constants)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every row of a glyph has the same length, and every character is `.` or an index into `ink`
 * @errors      noexcept
 * @frozen      no
 * @tests       theme.icons_are_well_formed, theme.icons_use_palette_ink
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace qp::views::qt::icons {

/// @brief How an icon's ink is coloured. The member names are `theme::Palette` fields.
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        none
/// @invariant   At most as many entries as `IconBitmap::kMaxInk`
/// @errors      noexcept
/// @frozen      no
/// @tests       theme.icons_use_palette_ink
enum class Ink : std::uint8_t {
    /// `Palette::text` -- the main stroke.
    stroke = 0,
    /// `Palette::accent` -- the part that carries the action's meaning.
    accent = 1,
    /// `Palette::warning` -- used only by the actions that need attention.
    warning = 2,
    /// `Palette::text_muted` -- a shadow or a secondary stroke.
    shade = 3,
};

/**
 * @brief One 8x8 glyph: eight equal-length rows of `.` and ink indices.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `rows` has `size` entries of equal length; `ink_count <= kMaxInk`
 * @errors      noexcept
 * @frozen      no
 * @tests       theme.icons_are_well_formed
 */
struct IconBitmap final {
    /// @brief The most ink roles one glyph may use, so the table below fits a fixed array.
    static constexpr std::size_t kMaxInk = 4;
    /// @brief Every glyph is this wide and this tall. A power of two, so scaling is exact.
    static constexpr std::size_t kSize = 8;

    const char* const* rows = nullptr;
    std::size_t size = 0;
    const Ink* ink = nullptr;
    std::size_t ink_count = 0;
};

/**
 * @brief Every glyph this UI ships, keyed by a stable id.
 *
 * The ids are the action names rather than numbers, because an icon that does not match its action is worse than
 * no icon: a user learns the glyph, and a wrong one teaches the wrong lesson.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A key appears at most once
 * @errors      noexcept
 * @frozen      no
 * @tests       theme.icons_are_well_formed
 */
enum class Glyph : std::uint8_t {
    new_document = 0,
    open = 1,
    save = 2,
    export_trace = 3,
    run = 4,
};

/// @brief The bitmap for a glyph. Total: an unknown glyph yields the `new_document` glyph rather than nothing.
///
/// @ownership   borrows (a reference to a function-local constant)
/// @thread      any
/// @pre         none
/// @post        `size == IconBitmap::kSize`
/// @invariant   Stable for the lifetime of the process
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
/// @tests       theme.icons_are_well_formed
[[nodiscard]] const IconBitmap& bitmap(Glyph glyph) noexcept;

/// @brief The machine name of a glyph, for a test's failure message and for `QAction::setObjectName`.
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        Non-empty for every glyph
/// @invariant   Distinct glyphs have distinct names
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
/// @tests       theme.icons_are_well_formed
[[nodiscard]] std::string_view name(Glyph glyph) noexcept;

/// @brief How many glyphs exist, so a generating test can iterate without a sentinel.
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        `>= 1`
/// @invariant   Constant
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
/// @tests       theme.icons_are_well_formed
[[nodiscard]] constexpr std::size_t count() noexcept { return 5; }

/// @brief The glyph for index `i`, or `new_document` past the end.
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        A valid glyph for every input
/// @invariant   `glyph(i) == i` for `i < count()`
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
/// @tests       theme.icons_are_well_formed
[[nodiscard]] constexpr Glyph glyph_at(std::size_t i) noexcept {
    return i < count() ? static_cast<Glyph>(i) : Glyph::new_document;
}

}  // namespace qp::views::qt::icons
