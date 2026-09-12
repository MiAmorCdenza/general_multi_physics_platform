/**
 * @file theme.cpp
 * @brief The palette, and the two measurements that make it a contract rather than a preference.
 */
#include "theme.hpp"

#include <QByteArray>
#include <QPainter>
#include <QPixmap>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace qp::views::qt::theme {
namespace {

/// @brief One sRGB channel as a fraction of full scale, which is what the transfer function takes.
[[nodiscard]] constexpr double unit(std::uint8_t channel) noexcept {
    return static_cast<double>(channel) / 255.0;
}

/// @brief The WCAG transfer function: gamma-decode one channel to linear light.
///
/// The break at 0.03928 and the 12.92 slope are the standard's, and they matter for dark colours: a palette
/// whose greys are all below the break would be measured incorrectly by the plain `pow(c, 2.4)` form, which is
/// the approximation everybody writes and which is wrong exactly where a dark theme lives.
[[nodiscard]] double to_linear(double c) noexcept {
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

/// @brief The inverse of `to_linear`, so a simulation can come back to sRGB.
[[nodiscard]] double to_srgb(double c) noexcept {
    const double clamped = std::clamp(c, 0.0, 1.0);
    return clamped <= 0.0031308 ? clamped * 12.92 : 1.055 * std::pow(clamped, 1.0 / 2.4) - 0.055;
}

[[nodiscard]] std::uint8_t to_channel(double linear_or_srgb) noexcept {
    const double v = std::clamp(linear_or_srgb, 0.0, 1.0);
    return static_cast<std::uint8_t>(std::lround(v * 255.0));
}

/// @brief The three channels of a colour as fractions.
struct Unit3 final {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

[[nodiscard]] Unit3 linear_of(Rgb c) noexcept {
    return Unit3{to_linear(unit(c.r)), to_linear(unit(c.g)), to_linear(unit(c.b))};
}

/// @brief Applies a 3x3 matrix in linear light and returns the sRGB result.
[[nodiscard]] Rgb apply(const std::array<std::array<double, 3>, 3>& m, Rgb c) noexcept {
    const Unit3 in = linear_of(c);
    const double r = m[0][0] * in.r + m[0][1] * in.g + m[0][2] * in.b;
    const double g = m[1][0] * in.r + m[1][1] * in.g + m[1][2] * in.b;
    const double b = m[2][0] * in.r + m[2][1] * in.g + m[2][2] * in.b;
    return Rgb{to_channel(to_srgb(r)), to_channel(to_srgb(g)), to_channel(to_srgb(b))};
}

/// @brief The names a palette groups by, matching the demonstrator library's `NodeDesc::category` values.
constexpr std::array<const char*, 4> kCategoryNames{"sources", "models", "instruments", "output"};

}  // namespace

double luminance(Rgb c) noexcept {
    const Unit3 lin = linear_of(c);
    return 0.2126 * lin.r + 0.7152 * lin.g + 0.0722 * lin.b;
}

double contrast(Rgb a, Rgb b) noexcept {
    const double la = luminance(a);
    const double lb = luminance(b);
    const double lighter = std::max(la, lb);
    const double darker = std::min(la, lb);
    return (lighter + 0.05) / (darker + 0.05);
}

Rgb simulate(Rgb c, Vision vision) noexcept {
    if (vision == Vision::normal) return c;
    // The standard linear-light dichromacy approximations (Vienot, Brettel and Mollon). Each matrix is what
    // "one cone does not respond" means once the remaining two have to reconstruct the missing axis; the
    // coefficients differ per model because the missing cone's contribution to each output channel does.
    switch (vision) {
        case Vision::protanopia: {
            static constexpr std::array<std::array<double, 3>, 3> m{{
                {0.152286, 1.052583, -0.204868},
                {0.114503, 0.786281, 0.099216},
                {-0.003882, -0.048116, 1.051998},
            }};
            return apply(m, c);
        }
        case Vision::deuteranopia: {
            static constexpr std::array<std::array<double, 3>, 3> m{{
                {0.367322, 0.860646, -0.227968},
                {0.280085, 0.672501, 0.047413},
                {-0.011820, 0.042940, 0.968881},
            }};
            return apply(m, c);
        }
        case Vision::tritanopia: {
            static constexpr std::array<std::array<double, 3>, 3> m{{
                {1.255528, -0.076749, -0.178779},
                {-0.078411, 0.930809, 0.147602},
                {0.004733, 0.691367, 0.303900},
            }};
            return apply(m, c);
        }
        case Vision::normal:
            break;
    }
    return c;
}

double delta(Rgb a, Rgb b) noexcept {
    const Unit3 x = linear_of(a);
    const Unit3 y = linear_of(b);
    const double dr = x.r - y.r;
    const double dg = x.g - y.g;
    const double db = x.b - y.b;
    // Weighted by the luminance coefficients rather than equally: two colours differing only in blue are much
    // harder to tell apart than the same numeric difference in green, and an unweighted distance would call
    // them equally distinguishable.
    return std::sqrt(0.2126 * dr * dr + 0.7152 * dg * dg + 0.0722 * db * db);
}

const Palette& dark() noexcept {
    // Measured, not chosen by eye, and the category colours are **searched** rather than picked: four hues whose
    // pairwise distance survives all three dichromacies, subject to every swatch clearing 3:1 against this
    // surface. The first attempt used a plausible-looking blue/green/amber/purple set, and
    // `theme.meaning_survives_colour_blindness` failed it -- sources and output measured 0.019 apart under
    // deuteranopia. The palette that passes measures 0.209, ten times the margin, and the search is recorded in
    // the commit message rather than kept as a script nobody would rerun.
    static const Palette p{
        /*surface             */ Rgb{0x1E, 0x21, 0x24},
        /*surface_raised      */ Rgb{0x33, 0x37, 0x3D},
        /*text                */ Rgb{0xE6, 0xE8, 0xEA},
        /*text_muted          */ Rgb{0x9A, 0xA2, 0xAC},
        /*text_disabled       */ Rgb{0x6B, 0x72, 0x7B},
        /*accent              */ Rgb{0x6C, 0xB0, 0xF0},
        /*on_accent           */ Rgb{0x10, 0x18, 0x20},
        /*warning             */ Rgb{0xD9, 0xA5, 0x20},
        /*on_warning          */ Rgb{0x20, 0x18, 0x00},
        /*category_sources    */ Rgb{0x65, 0x98, 0xE0},
        /*category_models     */ Rgb{0xE0, 0xDA, 0x92},
        /*category_instruments*/ Rgb{0xE0, 0xA3, 0x65},
        /*category_output     */ Rgb{0x8C, 0x66, 0x3F},
        /*edge                */ Rgb{0x7A, 0x84, 0x90},
        /*edge_dim            */ Rgb{0x55, 0x5B, 0x63},
    };
    return p;
}

const Palette& light() noexcept {
    // Its own search, because a swatch that reads well on `#1E2124` disappears on `#FAFAFA`. Best separation
    // 0.084 against 0.209 for the dark set, which is the honest consequence of a narrower lightness band.
    static const Palette p{
        /*surface             */ Rgb{0xFA, 0xFA, 0xFA},
        /*surface_raised      */ Rgb{0xFF, 0xFF, 0xFF},
        /*text                */ Rgb{0x1A, 0x1D, 0x20},
        /*text_muted          */ Rgb{0x4A, 0x51, 0x59},
        /*text_disabled       */ Rgb{0x8A, 0x92, 0x9A},
        /*accent              */ Rgb{0x1D, 0x5A, 0x9E},
        /*on_accent           */ Rgb{0xFF, 0xFF, 0xFF},
        /*warning             */ Rgb{0x7A, 0x5F, 0x00},
        /*on_warning          */ Rgb{0xFF, 0xFF, 0xFF},
        /*category_sources    */ Rgb{0x46, 0x63, 0x8C},
        /*category_models     */ Rgb{0x73, 0x6A, 0x06},
        /*category_instruments*/ Rgb{0x8C, 0x86, 0x46},
        /*category_output     */ Rgb{0x59, 0x38, 0x16},
        /*edge                */ Rgb{0x5A, 0x63, 0x6E},
        /*edge_dim            */ Rgb{0xB4, 0xBA, 0xC2},
    };
    return p;
}

const char* const* category_names(std::size_t& count) noexcept {
    count = kCategoryNames.size();
    return kCategoryNames.data();
}

Rgb category_colour(const Palette& p, const char* category) noexcept {
    const std::string_view name = category != nullptr ? std::string_view{category} : std::string_view{};
    if (name == kCategoryNames[0]) return p.category_sources;
    if (name == kCategoryNames[1]) return p.category_models;
    if (name == kCategoryNames[2]) return p.category_instruments;
    if (name == kCategoryNames[3]) return p.category_output;
    // Total, not a failure: an unrecognised category is a plugin's typo and the node still has to be drawn --
    // in the default group, where a user can see it and wonder, rather than not at all.
    return p.category_models;
}

Rgb text_on(const Palette& p, Rgb background) noexcept {
    // Whichever of the two text colours measures better on this fill. A computed answer rather than a rule like
    // "a dark fill gets light text", because "dark" and "light" are not properties a threshold can decide
    // reliably -- the luminance weighting is exactly the judgement being made, and `contrast` already
    // implements it.
    return contrast(p.text, background) >= contrast(p.on_accent, background) ? p.text : p.on_accent;
}

const Palette& palette() noexcept { return dark(); }

QColor to_qcolor(Rgb c) noexcept {
    return QColor{static_cast<int>(c.r), static_cast<int>(c.g), static_cast<int>(c.b)};
}

QColor category_colour(const QString& category) noexcept {
    const QByteArray utf8 = category.toUtf8();
    return to_qcolor(category_colour(palette(), utf8.constData()));
}

QColor text_on(Rgb fill) noexcept { return to_qcolor(theme::text_on(palette(), fill)); }

}  // namespace qp::views::qt::theme

namespace qp::views::qt {
namespace {

/// @brief The palette field an ink role names. Total: an unknown role falls back to the main stroke.
[[nodiscard]] QColor ink_colour(icons::Ink role) noexcept {
    const theme::Palette& p = theme::palette();
    switch (role) {
        case icons::Ink::stroke: return theme::to_qcolor(p.text);
        case icons::Ink::accent: return theme::to_qcolor(p.accent);
        case icons::Ink::warning: return theme::to_qcolor(p.warning);
        case icons::Ink::shade: return theme::to_qcolor(p.text_muted);
    }
    return theme::to_qcolor(p.text);
}

/// @brief The ink index a glyph character names, or -1 for transparent.
[[nodiscard]] int ink_index(char c) noexcept {
    return c >= '0' && c <= '9' ? c - '0' : -1;
}

}  // namespace

QIcon to_icon(icons::Glyph glyph, int size) noexcept {
    const icons::IconBitmap& art = icons::bitmap(glyph);
    const int wanted = size > 0 ? size : static_cast<int>(icons::IconBitmap::kSize);
    const int grid = static_cast<int>(icons::IconBitmap::kSize);
    // Whole-number scale, rounded up: a request of 10 must not become a 1x scale with two pixels clipped.
    const int scale = (wanted + grid - 1) / grid;

    // A `QImage`, not a `QPixmap`, and the difference is not cosmetic: a `QPixmap` requires a live
    // `QGuiApplication`, so an icon built into one can only be constructed from inside a running GUI -- which
    // would put this function out of reach of the suite that checks the glyphs, the whole reason the bitmaps are
    // data in the first place. `QPixmap::fromImage` needs no application object of its own.
    QImage image{grid * scale, grid * scale, QImage::Format_ARGB32};
    image.fill(Qt::transparent);
    QPainter painter{&image};
    for (std::size_t y = 0; y < art.size; ++y) {
        const char* row = art.rows[y];
        for (std::size_t x = 0; x < art.size; ++x) {
            const int index = ink_index(row[x]);
            if (index < 0 || static_cast<std::size_t>(index) >= art.ink_count) continue;
            painter.fillRect(QRect{static_cast<int>(x) * scale, static_cast<int>(y) * scale, scale, scale},
                             ink_colour(art.ink[index]));
        }
    }
    painter.end();
    return QIcon{QPixmap::fromImage(image)};
}

}  // namespace qp::views::qt
