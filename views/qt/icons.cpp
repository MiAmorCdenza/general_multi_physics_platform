/**
 * @file icons.cpp
 * @brief The glyph table: eight rows of eight characters each, and the ink each glyph uses.
 *
 * ## How to read a glyph
 *
 * `.` is transparent. A digit indexes the glyph's `ink` array. So:
 *
 *     "..0000.."
 *     ".011110."
 *
 * is a shape whose outline is `ink[0]` and whose interior is `ink[1]`.
 *
 * ## Why they look like this
 *
 * Eight by eight is the smallest grid on which a floppy disk, a folder and a play triangle are still
 * recognisable, and it is a power of two, so scaling to any of Qt's icon sizes is exact rather than interpolated
 * -- which is the whole point of pixel art. Each glyph is one idea: nothing here tries to be a picture.
 */
#include "icons.hpp"

#include <QImage>
#include <QString>

// The resource initialiser `AUTORCC` generates for `qp_icons.qrc`, declared here at **global** scope.
//
// `Q_INIT_RESOURCE` pastes its argument into a name and expands to a declaration plus a call, and inside a
// namespace the declaration it makes is namespaced while the generated definition is not -- which links as an
// undefined `qp::views::qt::icons::qInitResources_qp_icons`. Declaring it here and calling the global symbol by
// name is the portable spelling, and it keeps the reason in one place.
extern int qInitResources_qp_icons();

namespace qp::views::qt::icons {
namespace {

// -- new_document: a page with a folded corner and a plus ---------------------
const char* const kNewDocument[] = {
    ".000000.",
    ".011110.",
    ".011011.",
    ".011110.",
    ".0100.0.",
    ".0100.0.",
    ".011110.",
    ".000000.",
};
constexpr Ink kNewDocumentInk[] = {Ink::stroke, Ink::stroke, Ink::accent, Ink::shade};

// -- open: a folder, with the lid lifted --------------------------------------
const char* const kOpen[] = {
    "........",
    ".0000...",
    ".000000.",
    ".011110.",
    ".011110.",
    ".011110.",
    ".011110.",
    ".000000.",
};
constexpr Ink kOpenInk[] = {Ink::stroke, Ink::accent, Ink::shade, Ink::stroke};

// -- save: a floppy disk with a label window ----------------------------------
const char* const kSave[] = {
    ".000000.",
    ".011110.",
    ".011110.",
    ".000000.",
    ".022220.",
    ".022220.",
    ".000000.",
    "........",
};
constexpr Ink kSaveInk[] = {Ink::stroke, Ink::shade, Ink::accent, Ink::warning};

// -- export_trace: an arrow leaving a box -------------------------------------
const char* const kExport[] = {
    "...0....",
    "..000...",
    ".00000..",
    "..000...",
    "...0....",
    ".111111.",
    ".1....1.",
    ".111111.",
};
constexpr Ink kExportInk[] = {Ink::accent, Ink::stroke, Ink::shade, Ink::stroke};

// -- run: a play triangle -----------------------------------------------------
const char* const kRun[] = {
    ".0......",
    ".00.....",
    ".000....",
    ".0000...",
    ".0000...",
    ".000....",
    ".00.....",
    ".0......",
};
constexpr Ink kRunInk[] = {Ink::accent, Ink::stroke, Ink::stroke, Ink::stroke};

}  // namespace

const IconBitmap& bitmap(Glyph glyph) noexcept {
    static const IconBitmap kBitmaps[count()] = {
        {kNewDocument, IconBitmap::kSize, kNewDocumentInk, 4},
        {kOpen, IconBitmap::kSize, kOpenInk, 4},
        {kSave, IconBitmap::kSize, kSaveInk, 4},
        {kExport, IconBitmap::kSize, kExportInk, 4},
        {kRun, IconBitmap::kSize, kRunInk, 4},
    };
    const auto index = static_cast<std::size_t>(glyph);
    return kBitmaps[index < count() ? index : 0];
}

std::string_view name(Glyph glyph) noexcept {
    switch (glyph) {
        case Glyph::new_document: return "new_document";
        case Glyph::open: return "open";
        case Glyph::save: return "save";
        case Glyph::export_trace: return "export_trace";
        case Glyph::run: return "run";
    }
    return "unknown";
}

std::uint8_t Sheet::at(std::size_t x, std::size_t y) const noexcept {
    if (y >= height || x >= width) return 0xFFU;
    const std::size_t index = y * width + x;
    return index < pixels.size() ? pixels[index] : 0xFFU;
}

bool Sheet::glyph_has_ink(std::size_t g) const noexcept {
    const std::size_t left = g * kSheetStride;
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = left; x < left + IconBitmap::kSize && x < width; ++x) {
            if (at(x, y) != 0xFFU) return true;
        }
    }
    return false;
}

const Sheet& sheet() noexcept {
    // **This call is load-bearing.** The `.qrc` is compiled into `qp_views`, which is a STATIC library, and a
    // linker drops an object file nothing references -- so without a reference from inside the library the
    // resource is never registered and `QImage::load` fails at run time with no diagnostic at all. The
    // declaration above is what keeps the generated object alive.
    //
    // The `.qrc` is named `qp_icons.qrc`, not `icons.qrc`, because `Q_INIT_RESOURCE` pastes its argument into an
    // identifier: a resource called `icons` compiled beside `icons.cpp` gives two translation units the same
    // helper name, and the collision surfaces as an error naming the macro rather than the cause.
    (void)::qInitResources_qp_icons();

    static const Sheet loaded = [] {
        Sheet out;
        QImage image;
        if (!image.load(QString::fromLatin1(kSheetResource))) return out;

        // Converted to greyscale even if it already is: the file is 8-bit greyscale, and a build that shipped a
        // 32-bit copy would otherwise be read with `qRed` as the index -- a silent wrong answer rather than a
        // missing one. Reading the index from the red channel of a greyscale conversion is what makes the
        // loader indifferent to the file's channel count.
        if (image.format() != QImage::Format_Grayscale8) {
            image = image.convertToFormat(QImage::Format_Grayscale8);
        }
        if (image.isNull() || image.height() != static_cast<int>(IconBitmap::kSize)) return out;

        out.width = static_cast<std::size_t>(image.width());
        out.height = static_cast<std::size_t>(image.height());
        out.pixels.resize(out.width * out.height);
        for (std::size_t y = 0; y < out.height; ++y) {
            for (std::size_t x = 0; x < out.width; ++x) {
                const QRgb pixel = image.pixel(static_cast<int>(x), static_cast<int>(y));
                // Index 0xFF means transparent in the sheet, and index 0 is a real palette index -- so the
                // alpha channel decides, not the value. A sheet with an opaque 0xFF would read as opaque ink,
                // which is why the generator writes only 0..3 and 0xFF, and this keeps the distinction.
                out.pixels[y * out.width + x] =
                    qAlpha(pixel) == 0 ? 0xFFU : static_cast<std::uint8_t>(qRed(pixel));
            }
        }
        out.glyph_count = out.width / kSheetStride;
        if (out.width % kSheetStride != 0) ++out.glyph_count;
        return out;
    }();
    return loaded;
}

}  // namespace qp::views::qt::icons
