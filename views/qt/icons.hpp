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
 * @tests       theme.icons_are_well_formed, theme.icons_use_palette_ink,
 *              theme.icons_match_the_shipped_sheet
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace qp::views::qt::icons {

/**
 * @brief How an icon's ink is coloured. The member names are `theme::Palette` fields.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   At most as many entries as `IconBitmap::kMaxInk`
 * @errors      noexcept
 * @frozen      no
 * @tests       theme.icons_use_palette_ink
 */
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
    measure = 5,
};

/**
 * @brief The bitmap for a glyph. Total: an unknown glyph yields the `new_document` glyph rather than nothing.
 *
 * @ownership   borrows (a reference to a function-local constant)
 * @thread      any
 * @pre         none
 * @post        `size == IconBitmap::kSize`
 * @invariant   Stable for the lifetime of the process
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       theme.icons_are_well_formed
 */
[[nodiscard]] const IconBitmap& bitmap(Glyph glyph) noexcept;

/**
 * @brief The machine name of a glyph, for a test's failure message and for `QAction::setObjectName`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Non-empty for every glyph
 * @invariant   Distinct glyphs have distinct names
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       theme.icons_are_well_formed
 */
[[nodiscard]] std::string_view name(Glyph glyph) noexcept;

/**
 * @brief How many glyphs exist, so a generating test can iterate without a sentinel.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        `>= 1`
 * @invariant   Constant
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       theme.icons_are_well_formed
 */
[[nodiscard]] constexpr std::size_t count() noexcept { return 6; }

/**
 * @brief The glyph for index `i`, or `new_document` past the end.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        A valid glyph for every input
 * @invariant   `glyph(i) == i` for `i < count()`
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       theme.icons_are_well_formed
 */
[[nodiscard]] constexpr Glyph glyph_at(std::size_t i) noexcept {
    return i < count() ? static_cast<Glyph>(i) : Glyph::new_document;
}

/**
 * @brief The resource path of the shipped sprite sheet, inside `icons.qrc`.
 *
 * A resource rather than a file on disk: the sheet is part of the program, so an installed build cannot lose
 * it by being moved, and nothing has to know where the executable lives.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Non-null, starts with ':'
 * @invariant   Constant
 * @errors      noexcept
 * @frozen      no
 * @tests       theme.icons_match_the_shipped_sheet
 */
inline constexpr const char* kSheetResource = ":/qp/icons/icons.png";

/// @brief The gap between two glyphs in the sheet, in pixels. One transparent column.
inline constexpr std::size_t kSheetStride = IconBitmap::kSize + 1;

/**
 * @brief The sprite sheet, as one row of palette indices per glyph.
 *
 * Read from `kSheetResource`, which `icons.qrc` packages. The result is the **same shape as the bitmap table**:
 * `size()` glyphs of `IconBitmap::kSize` rows of `IconBitmap::kSize` bytes, each byte a palette index or `0xFF`
 * for transparent. Greyscale rather than a palette PNG, because the palette lives in `theme.hpp` and a PLTE
 * chunk in the file would be a second copy of it that nothing could keep in step.
 *
 * `pixels` is empty when the resource is missing or malformed -- a build that forgot to compile the `.qrc`. That
 * is a total answer rather than a crash, and the test that compares this against the table is what turns the
 * empty case into a failure with a name.
 *
 * @ownership   owns the returned sheet
 * @thread      ui
 * @pre         none
 * @post        Either empty, or `rows == kSize` and every glyph column is `kSize` wide
 * @invariant   Depends on nothing but the compiled-in resource
 * @errors      noexcept
 * @complexity  O(glyphs x size^2)
 * @nondet      none
 * @frozen      no
 * @tests       theme.icons_match_the_shipped_sheet
 */
struct Sheet final {
    /// One row of `glyph_count * kSize` bytes, `kSize` rows. Empty when unavailable.
    std::vector<std::uint8_t> pixels{};
    /// How many glyph columns the sheet holds.
    std::size_t glyph_count = 0;
    /// The sheet's own width in pixels, for a caller that wants to draw it whole.
    std::size_t width = 0;
    /// The sheet's own height in pixels.
    std::size_t height = 0;

    /// @brief Whether the sheet was read at all.
    [[nodiscard]] bool valid() const noexcept { return !pixels.empty(); }
    /// @brief Palette index at `(x, y)`, or `0xFF` when out of range.
    [[nodiscard]] std::uint8_t at(std::size_t x, std::size_t y) const noexcept;
    /// @brief Whether glyph column `g` has any opaque pixel.
    [[nodiscard]] bool glyph_has_ink(std::size_t g) const noexcept;
};

/**
 * @brief The shipped sheet, read once.
 *
 * @ownership   borrows (a reference to a function-local constant)
 * @thread      ui
 * @pre         none
 * @post        The same sheet every call
 * @invariant   Read once, because decoding a PNG per icon would be a per-paint cost
 * @errors      noexcept
 * @complexity  O(1) after the first call
 * @nondet      none
 * @frozen      no
 * @tests       theme.icons_match_the_shipped_sheet
 */
[[nodiscard]] const Sheet& sheet() noexcept;

}  // namespace qp::views::qt::icons
