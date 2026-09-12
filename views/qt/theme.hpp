/**
 * @file theme.hpp
 * @brief The projection-first palette: every colour the UI draws with, and the tests that make it a contract.
 *
 * ## Why the colours are Qt-free data
 *
 * Charter C5 requires the UI to be **high contrast, large type and colour-blind safe**, and it says that
 * requirement belongs to a view plugin's acceptance criteria. It was, until this file existed, only true by
 * inspection: the canvas, the panels and the warning text each held their own `QColor(0x..)` literals, so
 * "is the warning text readable" was answerable only by looking at a screenshot, and "is this palette
 * colour-blind safe" was not answerable at all.
 *
 * A colour is a value. WCAG contrast is arithmetic on three numbers, and a colour-vision-deficiency
 * simulation is a 3x3 matrix multiplication. Both run in the ordinary Catch2 suite, on **both** compilers,
 * with no toolkit and no event loop -- so the palette is checked rather than admired. What stays in `views/qt`
 * is the only thing that needs Qt: turning these values into `QColor`s and applying them.
 *
 * ## The measurements this file is written against
 *
 * WCAG 2.1 relative luminance, and the contrast ratio `(L1 + 0.05) / (L2 + 0.05)`. The thresholds are the
 * standard's, not this project's invention:
 *
 *   - **4.5:1** for body text (AA, normal size);
 *   - **3:1** for large text, graphical objects and user-interface components (AA).
 *
 * The palette is built to clear the **stricter** one for all text and for anything that carries meaning, which
 * is why `text_muted` is `#9aa2ac` rather than the `#6b7280` a modern dark theme would reach for: that one
 * measures about 3.6:1 against this background, which is fine for a large label and not fine for the 11 pt
 * notes the confidence panel draws.
 *
 * ## Colour blindness is not a matter of taste either
 *
 * The colour-vision-deficiency simulation is the Brettel/Vienot-style linear transform on linear-light RGB,
 * applied to the values below and measured with the same contrast function and the same `delta` distance. A
 * palette where two categories collapse into one under deuteranopia is one a colour-blind student cannot read,
 * and the way to know is to compute it. `theme.colours_survive_colour_blindness` does.
 *
 * @ownership   pure (constants and free functions)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every colour is a plain sRGB triple in [0, 255]
 * @errors      noexcept
 * @frozen      no
 * @tests       theme.contrast_is_measurable, theme.text_clears_wcag_aa,
 *              theme.meaning_survives_colour_blindness
 */
#pragma once

#include <QColor>
#include <QString>

#include <cstddef>
#include <cstdint>

namespace qp::views::qt::theme {

/**
 * @brief An sRGB colour: the value a painter is given, with no toolkit attached.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Channels are 0..255; alpha is not modelled because nothing in this UI is translucent
 * @errors      noexcept
 * @frozen      no
 */
struct Rgb final {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

/// @brief Structural equality, so a test can say "these two categories are the same colour".
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        none
/// @invariant   Compares all three channels
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
[[nodiscard]] constexpr bool operator==(Rgb a, Rgb b) noexcept {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

/// @brief The quantities a measurement reports, so the numbers are visible and not only asserted.
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        none
/// @invariant   `ratio >= 1.0` for any two colours
/// @errors      noexcept
/// @frozen      no
enum class Vision : std::uint8_t {
    /// Ordinary trichromatic vision.
    normal = 0,
    /// Red-blind. The commonest form in men, and the one a red/green pair fails.
    protanopia = 1,
    /// Green-blind. The other common form, and the one a red/green pair fails differently.
    deuteranopia = 2,
    /// Blue-blind. Rare, and it breaks pairs that a red/green check would pass.
    tritanopia = 3,
};

/// @brief Stable short name of a vision model, for a test's failure message.
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        Non-null for every enumerator
/// @invariant   Distinct models have distinct names
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
[[nodiscard]] constexpr const char* to_string(Vision v) noexcept {
    switch (v) {
        case Vision::normal: return "normal";
        case Vision::protanopia: return "protanopia";
        case Vision::deuteranopia: return "deuteranopia";
        case Vision::tritanopia: return "tritanopia";
    }
    return "unknown";
}

/**
 * @brief WCAG 2.1 relative luminance of an sRGB colour, in [0, 1].
 *
 * The standard's piecewise transfer function, applied per channel and combined with the standard weights. Not
 * the naive `(r+g+b)/3`: that measure calls pure blue and pure yellow equally bright, which is wrong by a
 * factor of twenty and would let a palette through that no one can read.
 *
 * @param c The colour.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The result is in [0, 1], and 0 exactly for black
 * @invariant   Depends only on `c`
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       theme.contrast_is_measurable
 */
[[nodiscard]] double luminance(Rgb c) noexcept;

/**
 * @brief The WCAG contrast ratio between two colours, from 1.0 (identical) to 21.0 (black on white).
 *
 * Symmetric in its arguments: the lighter colour supplies the numerator, so a caller does not have to know
 * which of the two is the background -- which is the mistake that makes a hand-written check report 1/4.5
 * instead of 4.5.
 *
 * @param a One colour.
 * @param b The other.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        `>= 1.0`, and `>= 4.5` exactly when the pair passes WCAG AA for body text
 * @invariant   Symmetric, and measured against `theme::contrast_minimum`
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       theme.contrast_is_measurable
 */
[[nodiscard]] double contrast(Rgb a, Rgb b) noexcept;

/// @brief The WCAG AA threshold for body text. The stricter of the two the standard defines.
inline constexpr double kBodyTextMinimum = 4.5;
/// @brief The WCAG AA threshold for large text, graphical objects and UI components.
inline constexpr double kGraphicsMinimum = 3.0;

/**
 * @brief How a colour appears to one of the three dichromacies.
 *
 * The transform is applied in **linear light** and converted back, which is what makes it a model of a
 * missing cone rather than a hue rotation: a linear-light matrix is what "this receptor does not respond"
 * actually means. Doing it in gamma-encoded sRGB is the common shortcut and it produces a plausible-looking
 * picture with the wrong distances, which is exactly the kind of error this file exists to avoid.
 *
 * @param c The colour.
 * @param vision The vision model. `normal` returns the colour unchanged.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        A colour in [0, 255] per channel; `normal` returns `c` exactly
 * @invariant   Two colours that are equal before the transform stay equal after it
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       theme.meaning_survives_colour_blindness
 */
[[nodiscard]] Rgb simulate(Rgb c, Vision vision) noexcept;

/**
 * @brief The perceptual distance between two colours, for "are these two distinguishable".
 *
 * A weighted Euclidean distance in linear light rather than a raw channel difference: `delta` is meant to
 * answer "would a reader tell these apart", and two colours that differ by 40 in the blue channel of a dark
 * pair are much harder to tell apart than the same 40 in red.
 *
 * Not a colour-difference standard (CIEDE2000 is), and deliberately not: the question here is only ever
 * "did these two categories collapse", and a cheap monotone measure answers it while staying readable in a
 * failure message.
 *
 * @param a One colour.
 * @param b The other.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        `>= 0`, and 0 exactly when the colours are equal
 * @invariant   Symmetric
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       theme.meaning_survives_colour_blindness
 */
[[nodiscard]] double delta(Rgb a, Rgb b) noexcept;

/**
 * @brief The dark palette: what the editor uses.
 *
 * Named fields rather than a list, so a missing colour is a compile error rather than a default. The names say
 * what the colour is **for**, not what it looks like: `text_muted` and `category_model` can both be grey and
 * neither can be replaced without thinking about why it exists.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every text colour in `text…` clears `kBodyTextMinimum` against `surface`
 * @errors      noexcept
 * @frozen      no
 */
struct Palette final {
    /// The canvas and window background. Everything else is measured against this.
    Rgb surface{};
    /// A raised panel: the node body, a table's alternating row.
    Rgb surface_raised{};
    /// Body text.
    Rgb text{};
    /// Secondary text: notes, units, hints. Still body text, so still `kBodyTextMinimum`.
    Rgb text_muted{};
    /// A disabled control's label. The **only** colour allowed below `kBodyTextMinimum`, because a disabled
    /// control is not information -- and it still clears `kGraphicsMinimum`.
    Rgb text_disabled{};
    /// Wiring, and the "this is selected" accent.
    Rgb accent{};
    /// Text drawn on top of `accent`.
    Rgb on_accent{};
    /// The marks that say something needs the user's attention: a gap list, a clamp count.
    Rgb warning{};
    /// Text drawn on top of `warning`.
    Rgb on_warning{};
    /// One colour per node category, so the palette can group them. Four because the demonstrator library has
    /// four, and a bigger set is a decision to make when a fifth category exists rather than in advance.
    Rgb category_sources{};
    Rgb category_models{};
    Rgb category_instruments{};
    Rgb category_output{};
    /// A connection that is part of the selected node's wiring.
    Rgb edge{};
    /// A connection that is not.
    Rgb edge_dim{};
};

/// @brief The dark palette. The one the editor uses, and the one C5's thresholds are asserted against.
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        none
/// @invariant   Constant
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
[[nodiscard]] const Palette& dark() noexcept;

/// @brief The light palette, for a projector or a printed screenshot.
///
/// Present because a lab machine is often a projector, and because having two palettes is what forces the
/// thresholds to be a **test** rather than a property of one lucky set of numbers. Same invariants.
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        none
/// @invariant   Constant
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
[[nodiscard]] const Palette& light() noexcept;

/**
 * @brief The category colours of a palette, in the order a palette widget groups them.
 *
 * Returned as a vector-of-pairs rather than one array, so a test can name which two categories collapsed
 * instead of reporting an index. The names match `NodeDesc::category` for the demonstrator library, which is
 * what a palette groups by.
 *
 * @param p The palette to read.
 *
 * @ownership   owns the returned vector
 * @thread      any
 * @pre         none
 * @post        One entry per category, names unique
 * @invariant   The same order for every palette
 * @errors      noexcept; no allocation, because the table is a static array
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       theme.meaning_survives_colour_blindness,
 *              theme.an_unknown_category_still_gets_a_colour
 */
[[nodiscard]] const char* const* category_names(std::size_t& count) noexcept;

/**
 * @brief The colour for a node category, or the model colour when the category is unknown.
 *
 * A total function rather than a lookup that can fail: an unrecognised category is a plugin's typo, and the
 * palette must still draw the node -- in the default group, which is where a user can see it and wonder.
 *
 * @param p The palette.
 * @param category The `NodeDesc::category` value. Empty and unknown values both fall back.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        One of the four category colours
 * @invariant   Depends only on its arguments
 * @errors      noexcept
 * @complexity  O(names)
 * @nondet      none
 * @frozen      no
 * @tests       theme.an_unknown_category_still_gets_a_colour
 */
[[nodiscard]] Rgb category_colour(const Palette& p, const char* category) noexcept;

/**
 * @brief The text colour to draw on top of `background`.
 *
 * ## Why this exists, and what it fixed
 *
 * The canvas tints a node box with its category colour and writes the node's title inside it. That is a
 * foreground/background pair the palette never declared, so nothing checked it -- and a screenshot review of the
 * first themed build found the subtitle washed out on exactly one of the three node groups. The fix is not to
 * pick a different grey by eye; it is to make "what do I draw on this" a question with a computed answer.
 *
 * `text` and `on_accent` are the same colour family, and `text_disabled` is too dark for a mid-tone fill, so the
 * choice is between `text` and the darker `on_accent`. Returns whichever measures **better** against
 * `background`, which for every category colour in both palettes means `text` -- and for the fill the selected
 * node gets, whichever it actually is.
 *
 * @param p The palette.
 * @param background The fill a label will be drawn on.
 *
 * @ownership   pure
 * @thread      ui
 * @pre         none
 * @post        One of the palette's two text colours, whichever contrasts better
 * @invariant   The result clears `kBodyTextMinimum` against `background` for every category colour of every
 *              palette -- asserted by `theme.text_on_a_tinted_node_is_readable`
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       theme.text_on_a_tinted_node_is_readable
 */
[[nodiscard]] Rgb text_on(const Palette& p, Rgb background) noexcept;

/**
 * @brief The palette this UI draws with.
 *
 * One function, so switching to the light palette for a projector is a one-line change rather than a search
 * through four files for `QColor(0x..)` literals. Each of those literals was a colour with no definition: "is
 * the warning text readable" was answerable only by looking at a screenshot.
 *
 * @ownership   borrows (a reference to a function-local constant)
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   The same palette for the lifetime of the process
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       theme.text_clears_wcag_aa
 */
[[nodiscard]] const Palette& palette() noexcept;

/**
 * @brief A theme colour as Qt's colour type: the one thing here that needs a toolkit.
 *
 * Everything else in this header is arithmetic on three bytes, which is why the palette can be asserted in the
 * ordinary suite with no event loop. This function is the boundary, and it is deliberately this small.
 *
 * @ownership   owns the returned value
 * @thread      ui
 * @pre         none
 * @post        An opaque colour with the same channels
 * @invariant   Round-trips through `QColor::rgb()` unchanged
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       theme.contrast_is_measurable
 */
[[nodiscard]] QColor to_qcolor(Rgb c) noexcept;

/**
 * @brief The colour for a node category, as the palette widget and the canvas both want it.
 *
 * @ownership   owns the returned value
 * @thread      ui
 * @pre         none
 * @post        One of the palette's category colours
 * @invariant   Agrees with `category_colour` for every input, including an unknown one
 * @errors      noexcept
 * @complexity  O(names)
 * @nondet      none
 * @frozen      no
 * @tests       theme.meaning_survives_colour_blindness
 */
[[nodiscard]] QColor category_colour(const QString& category) noexcept;

/**
 * @brief The text colour to draw on top of `fill`, for a painter.
 *
 * The Qt-facing half of `text_on`, and the one the canvas actually calls: a node box is tinted with its category
 * colour and its labels are drawn inside it, which is a foreground/background pair the palette would otherwise
 * never have declared. See `text_on` for the defect that made this necessary.
 *
 * @ownership   owns the returned value
 * @thread      ui
 * @pre         none
 * @post        Text that clears `kBodyTextMinimum` against `fill`
 * @invariant   Agrees with `text_on` for every input
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       theme.text_on_a_tinted_node_is_readable
 */
[[nodiscard]] QColor text_on(Rgb fill) noexcept;

}  // namespace qp::views::qt::theme
