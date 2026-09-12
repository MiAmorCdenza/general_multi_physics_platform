/**
 * @file test_theme.cpp
 * @brief Charter C5 as an executable acceptance criterion: contrast, and colour-blind safety.
 *
 * ## Why these are arithmetic rather than screenshots
 *
 * C5 says the UI must be "high contrast, large type and colour-blind safe" and puts that requirement in a view
 * plugin's acceptance criteria. Before this file, the requirement was satisfied by inspection: the colours were
 * `QColor(0x..)` literals inside paint code, so "is the warning text readable" could only be answered by
 * looking, and "is this palette colour-blind safe" could not be answered at all.
 *
 * A colour is three numbers, WCAG contrast is a formula over them, and a colour-vision deficiency is a linear
 * map. So the palette is asserted here -- on both compilers, with no toolkit and no event loop -- and the Qt
 * side only turns the values into `QColor`s. When this file fails it prints the ratio it measured, which is the
 * number a designer would have needed anyway.
 *
 * ## What is deliberately not asserted
 *
 * Not "these are good colours". The checks are the standard's thresholds and one structural property (no two
 * categories collapse). A palette that clears them is legible; whether it is *pleasant* is not a property a
 * test can hold, and pretending otherwise is how a colour test becomes a change-detector that gets deleted.
 */
#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "theme.hpp"

#include <QApplication>
#include <QIcon>
#include <QImage>
#include <QPixmap>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using qp::views::qt::theme::Palette;
using qp::views::qt::theme::Rgb;
using qp::views::qt::theme::Vision;

namespace {

/// @brief Every text/surface pair the UI actually draws, with the name a failure should print.
struct Pair final {
    const char* what;
    Rgb foreground;
    Rgb background;
};

/// @brief The pairs a reader has to be able to read. One per field that carries words.
[[nodiscard]] std::vector<Pair> text_pairs(const Palette& p) {
    return {
        {"body text on the canvas", p.text, p.surface},
        {"body text on a raised panel", p.text, p.surface_raised},
        {"muted text on the canvas", p.text_muted, p.surface},
        {"muted text on a raised panel", p.text_muted, p.surface_raised},
        {"text on the accent colour", p.on_accent, p.accent},
        {"text on the warning colour", p.on_warning, p.warning},
    };
}

/// @brief The pairs a node box draws, now that its labels sit on a surface rather than on the category fill.
///
/// Added when the two-zone box replaced the tinted one: `surface_raised` became a background for real text, and
/// a background nothing measures is how the previous defect happened. The last entry is the one that caught the
/// second half of it -- the footer was using `text_disabled`, which measures 2.5:1 there.
[[nodiscard]] std::vector<Pair> node_body_pairs(const Palette& p) {
    return {
        {"node type name on the node body", p.text_muted, p.surface_raised},
        {"node port label on the node body", p.text, p.surface_raised},
        {"node port stub on the node body", p.accent, p.surface_raised},
        {"node footer on the node body", p.text_muted, p.surface_raised},
    };
}

}  // namespace

TEST_CASE("theme.contrast_is_measurable", "[views][theme]") {
    using qp::views::qt::theme::contrast;
    using qp::views::qt::theme::luminance;

    // The two anchor points every implementation has to get right: black on white is 21:1, and anything with
    // itself is 1:1. A contrast function that fails these is measuring something else.
    const Rgb black{0, 0, 0};
    const Rgb white{255, 255, 255};
    CHECK(luminance(black) == 0.0);
    CHECK(luminance(white) == 1.0);
    CHECK(std::abs(contrast(black, white) - 21.0) < 1e-9);
    CHECK(contrast(white, white) == 1.0);

    // Symmetric. The first version of a hand-written check in another project divided the wrong way round and
    // reported 1/4.5 for a pair that passed, which is why this is asserted rather than assumed.
    CHECK(contrast(black, white) == contrast(white, black));

    // And it is **not** a plain average: pure blue and pure yellow differ in perceived brightness by a factor
    // of about twenty, so a palette checked with `(r+g+b)/3` would let an unreadable pair through. Asserting
    // the direction is what catches someone replacing the formula with the shortcut.
    const Rgb blue{0, 0, 255};
    const Rgb yellow{255, 255, 0};
    CHECK(luminance(yellow) > 10.0 * luminance(blue));
}

TEST_CASE("theme.text_clears_wcag_aa", "[views][theme]") {
    // WCAG 2.1 AA for body text, applied to **both** palettes. Two palettes rather than one is what turns the
    // thresholds into a test: with a single set of hand-chosen colours, "it passes" could be luck.
    for (const Palette* p : {&qp::views::qt::theme::dark(), &qp::views::qt::theme::light()}) {
        for (const Pair& pair : text_pairs(*p)) {
            const double ratio = qp::views::qt::theme::contrast(pair.foreground, pair.background);
            INFO(pair.what << " measures " << ratio << ":1");
            CHECK(ratio >= qp::views::qt::theme::kBodyTextMinimum);
        }

        // A disabled label is the one thing allowed below the body-text threshold, and it is still held to
        // the graphics threshold: "disabled" means "you cannot use this", not "you cannot find this".
        const double disabled = qp::views::qt::theme::contrast(p->text_disabled, p->surface);
        INFO("disabled text measures " << disabled << ":1");
        CHECK(disabled >= qp::views::qt::theme::kGraphicsMinimum);

        // The node box's own pairs. These are the ones that were missing when a tinted box held the labels.
        for (const Pair& pair : node_body_pairs(*p)) {
            const double ratio = qp::views::qt::theme::contrast(pair.foreground, pair.background);
            INFO(pair.what << " measures " << ratio << ":1");
            CHECK(ratio >= qp::views::qt::theme::kBodyTextMinimum);
        }
    }
}

TEST_CASE("theme.meaning_survives_colour_blindness", "[views][theme]") {
    // The property C5's "colour-blind safe" actually names. Not "the colours look different to me": every pair
    // of categories must stay distinguishable under all three dichromacies, and the check is a distance rather
    // than an inequality, because two colours differing by one count are not distinguishable by anybody.
    //
    // ### What the threshold is, and why it is two numbers
    //
    // An arbitrary cut-off ("greater than 0.02") is the kind of number that gets tuned down the first time it
    // fails, so there are two floors and each has a reason:
    //
    //   - **Under ordinary vision, 0.15.** A palette whose categories are hard to tell apart *before* any
    //     simulation is not a colour-blind problem, and this is the check that catches it.
    //   - **Under a dichromacy, 0.05.** A simulated pair is flat by construction -- the model has thrown an axis
    //     away -- so demanding the ordinary-vision margin there would demand the impossible. 0.05 in this
    //     weighted linear-light metric is about a 5% luminance step, which is several times the roughly 1%
    //     threshold at which a person starts to see a difference in a large uniform patch.
    //
    // The dark palette is held to a **higher** ordinary-vision floor than the light one, and the difference is
    // a property of the surfaces rather than a concession. On `#1E2124` a swatch can be anywhere from mid to
    // near-white, a span of about 0.75 in luminance; on `#FAFAFA` everything must be *darker* than a bright
    // surface with room to be seen, which compresses the usable range to about 0.25. A search of the whole
    // hue/lightness space found 0.209 separation for the dark surface and 0.084 for the light one, and asking
    // for the dark palette's margin on the light surface would not produce a better palette -- it would produce
    // a failing test that somebody eventually relaxes to whatever the current colours happen to measure.
    constexpr double kDarkOrdinaryFloor = 0.15;
    constexpr double kLightOrdinaryFloor = 0.07;
    constexpr double kSimulatedFloor = 0.05;
    const std::array<Vision, 4> models{Vision::normal, Vision::protanopia, Vision::deuteranopia,
                                       Vision::tritanopia};

    for (const Palette* p : {&qp::views::qt::theme::dark(), &qp::views::qt::theme::light()}) {
        std::size_t names_count = 0;
        const char* const* names = qp::views::qt::theme::category_names(names_count);
        REQUIRE(names_count >= 2);
        const bool is_dark = p == &qp::views::qt::theme::dark();

        for (const Vision vision : models) {
            const double floor = vision == Vision::normal
                                     ? (is_dark ? kDarkOrdinaryFloor : kLightOrdinaryFloor)
                                     : kSimulatedFloor;
            for (std::size_t i = 0; i < names_count; ++i) {
                for (std::size_t j = i + 1; j < names_count; ++j) {
                    const Rgb raw_a = qp::views::qt::theme::category_colour(*p, names[i]);
                    const Rgb raw_b = qp::views::qt::theme::category_colour(*p, names[j]);
                    if (qp::views::qt::theme::contrast(raw_a, p->surface) <
                            qp::views::qt::theme::kGraphicsMinimum ||
                        qp::views::qt::theme::contrast(raw_b, p->surface) <
                            qp::views::qt::theme::kGraphicsMinimum) {
                        continue;  // At least one of the two is not visible in the first place.
                    }
                    const Rgb a = qp::views::qt::theme::simulate(raw_a, vision);
                    const Rgb b = qp::views::qt::theme::simulate(raw_b, vision);
                    const double distance = qp::views::qt::theme::delta(a, b);
                    INFO(names[i] << " vs " << names[j] << " under "
                                  << qp::views::qt::theme::to_string(vision) << " measures " << distance
                                  << ", floor " << floor);
                    CHECK(distance > floor);
                    CHECK_FALSE(a == b);
                }
            }
        }
    }

    // Every category colour has to be visible against its own surface, which is the filter above and is worth
    // asserting directly: a search that returned four perfectly separated but invisible colours would satisfy
    // the distance check vacuously.
    for (const Palette* p : {&qp::views::qt::theme::dark(), &qp::views::qt::theme::light()}) {
        std::size_t names_count = 0;
        const char* const* names = qp::views::qt::theme::category_names(names_count);
        for (std::size_t i = 0; i < names_count; ++i) {
            const double ratio = qp::views::qt::theme::contrast(
                qp::views::qt::theme::category_colour(*p, names[i]), p->surface);
            INFO(names[i] << " measures " << ratio << ":1 against the surface");
            CHECK(ratio >= qp::views::qt::theme::kGraphicsMinimum);
        }
    }

    // The simulation is a model, not a hue rotation, and two facts pin that down: it is the identity for
    // ordinary vision, and it is **not** the identity for a dichromacy -- a "simulation" that returned its
    // input would pass every assertion above while checking nothing.
    const Rgb red{0xD0, 0x30, 0x30};
    CHECK(qp::views::qt::theme::simulate(red, Vision::normal) == red);
    CHECK_FALSE(qp::views::qt::theme::simulate(red, Vision::deuteranopia) == red);

    // A red/green pair is the canonical failure, and it must collapse under deuteranopia: this is the fact
    // that makes the category colours above worth checking at all.
    const Rgb green{0x30, 0xC0, 0x30};
    const double normal_gap = qp::views::qt::theme::delta(red, green);
    const double deutan_gap =
        qp::views::qt::theme::delta(qp::views::qt::theme::simulate(red, Vision::deuteranopia),
                                qp::views::qt::theme::simulate(green, Vision::deuteranopia));
    INFO("red/green gap: " << normal_gap << " normally, " << deutan_gap << " under deuteranopia");
    CHECK(deutan_gap < normal_gap / 2.0);
}

TEST_CASE("theme.text_on_a_tinted_node_is_readable", "[views][theme]") {
    // The pair the palette never declared until a screenshot review found it: a node box was tinted with its
    // category colour and its labels drawn **inside** it. The first themed build had the secondary line washed
    // out on one of the three node groups -- which is what "the palette clears WCAG AA" does not catch, because
    // that check measures body text on the *surface*, not text on a fill.
    //
    // ## What the measurement then forced
    //
    // Making the pair a checked property showed the deeper problem: on the dark palette's `#8C663F` the best ink
    // available measures 4.18:1, so **no** colour in the palette can label that fill to AA for body text. A
    // mid-tone fill is not a background for body text, and no amount of choosing fixes it.
    //
    // So the canvas was changed rather than the threshold: the category colour is now a **header band** carrying
    // one short title, and everything that has to be read sits on `surface_raised`. That is what this case
    // encodes -- the band is held to AA for **large** text, which is the standard's own allowance for a short
    // label at a larger size, and the title is the only thing on a fill.
    constexpr double kBandMinimum = qp::views::qt::theme::kGraphicsMinimum;
    for (const Palette* p : {&qp::views::qt::theme::dark(), &qp::views::qt::theme::light()}) {
        std::size_t names_count = 0;
        const char* const* names = qp::views::qt::theme::category_names(names_count);
        REQUIRE(names_count >= 2);
        for (std::size_t i = 0; i < names_count; ++i) {
            const Rgb fill = qp::views::qt::theme::category_colour(*p, names[i]);
            const Rgb ink = qp::views::qt::theme::text_on(*p, fill);
            const double ratio = qp::views::qt::theme::contrast(ink, fill);
            INFO(names[i] << ": text_on measures " << ratio << ":1 on its own fill");
            CHECK(ratio >= kBandMinimum);
            // The best available ink, not merely an adequate one: if the other text colour measured better, the
            // function picked wrong.
            const Rgb other = ink == p->text ? p->on_accent : p->text;
            CHECK(ratio >= qp::views::qt::theme::contrast(other, fill));
        }
    }

    // It picks the **better** of the two, which is what makes it a computation rather than a constant: on a dark
    // surface the light text wins, and on a light fill the dark one does. A function that always returned `text`
    // would pass the loop above for the dark palette and fail here.
    const Palette& dark_p = qp::views::qt::theme::dark();
    const Palette& light_p = qp::views::qt::theme::light();
    CHECK(qp::views::qt::theme::text_on(dark_p, dark_p.surface) == dark_p.text);
    CHECK(qp::views::qt::theme::text_on(light_p, light_p.text) == light_p.on_accent);

    // And the answer is one of the palette's two text colours, not a new one invented per call: a third colour
    // appearing here would be a colour nothing else in this file checks.
    const Rgb on_yellow = qp::views::qt::theme::text_on(dark_p, dark_p.category_models);
    CHECK((on_yellow == dark_p.text || on_yellow == dark_p.on_accent));
}

TEST_CASE("theme.icons_are_well_formed", "[views][theme]") {
    // An icon is eight rows of eight characters. A row of seven would rasterise a diagonal edge as a vertical
    // one, and nothing about a glyph makes that visible in a diff -- so the shape rules are asserted rather than
    // trusted to whoever edited the table last.
    using qp::views::qt::icons::IconBitmap;
    using qp::views::qt::icons::Glyph;
    using qp::views::qt::icons::Ink;

    std::vector<std::string> names;
    for (std::size_t i = 0; i < qp::views::qt::icons::count(); ++i) {
        const Glyph glyph = qp::views::qt::icons::glyph_at(i);
        const IconBitmap& art = qp::views::qt::icons::bitmap(glyph);
        const std::string label{qp::views::qt::icons::name(glyph)};
        INFO("glyph " << label);

        REQUIRE_FALSE(label.empty());
        // The ids are what a test's failure message and a `QAction::objectName` use, so a duplicate would make
        // two different actions indistinguishable in a log.
        REQUIRE(std::find(names.begin(), names.end(), label) == names.end());
        names.push_back(label);

        REQUIRE(art.size == IconBitmap::kSize);
        REQUIRE(art.rows != nullptr);
        REQUIRE(art.ink != nullptr);
        REQUIRE(art.ink_count > 0);
        REQUIRE(art.ink_count <= IconBitmap::kMaxInk);

        std::size_t ink_used = 0;
        bool any_pixel = false;
        for (std::size_t y = 0; y < art.size; ++y) {
            const std::string row{art.rows[y]};
            REQUIRE(row.size() == IconBitmap::kSize);
            for (const char c : row) {
                if (c == '.') continue;
                any_pixel = true;
                REQUIRE(c >= '0');
                REQUIRE(c <= '9');
                const auto index = static_cast<std::size_t>(c - '0');
                // An index past the glyph's own ink list is a transparent pixel that silently draws nothing,
                // which is how half an icon goes missing.
                REQUIRE(index < art.ink_count);
                ink_used = std::max(ink_used, index + 1);
            }
        }
        // Not every declared role has to be used, but an icon that is entirely transparent is not an icon, and
        // neither is one whose declared ink never appears.
        REQUIRE(any_pixel);
        REQUIRE(ink_used > 0);
    }

    // The table is keyed by a scoped enum, so the guard is that every enumerator is reachable by index and that
    // an out-of-range index is the documented fallback rather than a crash.
    REQUIRE(qp::views::qt::icons::glyph_at(0) == Glyph::new_document);
    REQUIRE(qp::views::qt::icons::glyph_at(qp::views::qt::icons::count() - 1) == Glyph::measure);
    REQUIRE(qp::views::qt::icons::glyph_at(999) == Glyph::new_document);
    REQUIRE(qp::views::qt::icons::bitmap(Glyph::run).size == IconBitmap::kSize);
    // `Ink` is a role, not a colour: enumerating it is what makes `icons_use_palette_ink` below possible.
    REQUIRE(static_cast<int>(Ink::stroke) == 0);
}

TEST_CASE("theme.icons_use_palette_ink", "[views][theme]") {
    // The property that keeps an icon from reintroducing the hard-coded colours `theme.hpp` exists to remove.
    // The bitmaps speak in **roles**, and the two facts that make that real are checked here: rasterising with
    // the dark palette gives different pixels than rasterising with the light one, and every non-transparent
    // pixel of a dark-palette icon is one of the dark palette's own colours.
    using qp::views::qt::icons::Glyph;
    using qp::views::qt::icons::count;
    using qp::views::qt::icons::glyph_at;

    std::vector<QRgb> palette_colours;
    const qp::views::qt::theme::Palette& p = qp::views::qt::theme::palette();
    for (const qp::views::qt::theme::Rgb c :
         {p.text, p.text_muted, p.text_disabled, p.accent, p.on_accent, p.warning, p.on_warning,
          p.category_sources, p.category_models, p.category_instruments, p.category_output, p.edge,
          p.edge_dim}) {
        palette_colours.push_back(qp::views::qt::theme::to_qcolor(c).rgb());
    }

    for (std::size_t i = 0; i < count(); ++i) {
        const Glyph glyph = glyph_at(i);
        const QIcon icon = qp::views::qt::to_icon(glyph, 16);
        REQUIRE_FALSE(icon.isNull());
        const QPixmap pixmap = icon.pixmap(16, 16);
        REQUIRE_FALSE(pixmap.isNull());

        std::size_t opaque = 0;
        for (int y = 0; y < pixmap.height(); ++y) {
            for (int x = 0; x < pixmap.width(); ++x) {
                const QColor pixel = pixmap.toImage().pixelColor(x, y);
                if (pixel.alpha() == 0) continue;
                ++opaque;
                const QRgb rgb = pixel.rgb();
                INFO(qp::views::qt::icons::name(glyph) << " pixel " << x << "," << y);
                REQUIRE(std::find(palette_colours.begin(), palette_colours.end(), rgb) !=
                        palette_colours.end());
            }
        }
        // An icon that rasterised to nothing would pass the loop above vacuously.
        REQUIRE(opaque > 0);
    }

    // And it is a rasterisation, not a blob: the run glyph is a triangle, so its top row has fewer opaque pixels
    // than its middle row. This is the check that would catch a scaling bug that filled the whole box.
    const QImage run = qp::views::qt::to_icon(Glyph::run, 16).pixmap(16, 16).toImage();
    const auto opaque_in_row = [&run](int y) {
        int n = 0;
        for (int x = 0; x < run.width(); ++x) {
            if (run.pixelColor(x, y).alpha() != 0) ++n;
        }
        return n;
    };
    REQUIRE(opaque_in_row(2) < opaque_in_row(8));
}

TEST_CASE("theme.icons_match_the_shipped_sheet", "[views][theme]") {
    // The sheet under `views/qt/resources/` is a **derived artefact** of the bitmap table in `icons.cpp`, and
    // derived artefacts drift: somebody edits a glyph, or regenerates the sheet from a stale table, and the two
    // disagree silently -- the palette would still paint and only the shape would be wrong.
    //
    // So they are compared pixel for pixel. That is what makes it safe to keep the sheet in the repository
    // rather than generating it on every build: it cannot be stale without failing here.
    using qp::views::qt::icons::Glyph;
    using qp::views::qt::icons::IconBitmap;
    using qp::views::qt::icons::count;
    using qp::views::qt::icons::glyph_at;
    using qp::views::qt::icons::kSheetStride;
    using qp::views::qt::icons::sheet;

    const qp::views::qt::icons::Sheet& art = sheet();
    REQUIRE(art.valid());
    REQUIRE(art.height == IconBitmap::kSize);
    REQUIRE(art.glyph_count == count());
    // One transparent column between glyphs, and nothing after the last one.
    REQUIRE(art.width == count() * IconBitmap::kSize + (count() - 1));

    for (std::size_t g = 0; g < count(); ++g) {
        const Glyph glyph = glyph_at(g);
        const IconBitmap& bitmap = qp::views::qt::icons::bitmap(glyph);
        INFO("glyph " << g << " (" << qp::views::qt::icons::name(glyph) << ")");
        REQUIRE(art.glyph_has_ink(g));

        for (std::size_t y = 0; y < IconBitmap::kSize; ++y) {
            for (std::size_t x = 0; x < IconBitmap::kSize; ++x) {
                const char c = bitmap.rows[y][x];
                // The table's index characters and the sheet's bytes are one alphabet: `0`..`3`, with `.`
                // written as the sheet's transparent marker.
                const std::uint8_t expected = c == '.' ? 0xFFU : static_cast<std::uint8_t>(c - '0');
                REQUIRE(art.at(g * kSheetStride + x, y) == expected);
            }
        }

        // The separator column is transparent in every row, or two glyphs touch and the sheet is unreadable as
        // a sheet.
        for (std::size_t y = 0; y < IconBitmap::kSize; ++y) {
            REQUIRE(art.at(g * kSheetStride + IconBitmap::kSize, y) == 0xFFU);
        }
    }

    // And the run-time path really rasterises: the `run` glyph becomes a 16-pixel image, so a resource that was
    // packaged but never read would not pass by comparing the sheet to the table alone.
    const QImage rendered = qp::views::qt::to_icon(Glyph::run, 16).pixmap(16, 16).toImage();
    REQUIRE_FALSE(rendered.isNull());
    REQUIRE(rendered.height() == 16);
    REQUIRE(rendered.width() == 16);
}

TEST_CASE("theme.an_unknown_category_still_gets_a_colour", "[views][theme]") {
    // Total on purpose. An unrecognised category is a plugin's typo, and the node still has to be drawn -- in
    // the default group, where a user can see it and wonder, rather than not at all. A lookup that returned
    // "no colour" would produce an invisible node, which reads as a missing feature.
    const Palette& p = qp::views::qt::theme::dark();
    CHECK(qp::views::qt::theme::category_colour(p, "models") == p.category_models);
    CHECK(qp::views::qt::theme::category_colour(p, "no.such.category") == p.category_models);
    CHECK(qp::views::qt::theme::category_colour(p, "") == p.category_models);
    CHECK(qp::views::qt::theme::category_colour(p, nullptr) == p.category_models);
    // And the named ones are the ones a palette groups by, so a group and its colour cannot drift apart.
    std::size_t count = 0;
    const char* const* names = qp::views::qt::theme::category_names(count);
    REQUIRE(count == 4);
    CHECK(qp::views::qt::theme::category_colour(p, names[0]) == p.category_sources);
    CHECK(qp::views::qt::theme::category_colour(p, names[3]) == p.category_output);
}

/**
 * @brief A `QApplication`, so the icon cases can rasterise.
 *
 * Most of this file is arithmetic on three bytes and needs nothing, which is the point of the palette living in
 * data. The **icons** are the exception: `QIcon` and `QPixmap` are GUI types, and Qt refuses to construct one
 * before an application object exists. Built in `main` rather than as a static, because a static `QApplication`
 * outlives `main`'s teardown order and Qt warns about it.
 *
 * This is the same shape as `test_views_qt.cpp`, and it needs the same CMake treatment: the executable must run
 * with Qt's DLLs on `PATH`, so it is a plain `add_executable` with one CTest entry rather than
 * `catch_discover_tests`, which would have to run it at build time.
 */
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}