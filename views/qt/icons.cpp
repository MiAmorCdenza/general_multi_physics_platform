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

}  // namespace qp::views::qt::icons
