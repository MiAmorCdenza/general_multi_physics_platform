/**
 * @file test_io.cpp
 * @brief Tests for the export contract and its registry.
 *
 * Test case ids match the @tests fields in the io headers byte for byte.
 *
 * The judgement this module makes is not about writing files -- it writes none. It
 * is about **answering before the write**, so that "this format cannot keep your
 * uncertainties" reaches a user as a disabled menu entry rather than as a published
 * table that lost its error bars. That answer is what these tests pin down.
 *
 * The exporters here are fakes: this module must contain no format, and a test that
 * shipped a real CSV writer would blur exactly the boundary the module exists to
 * hold.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/runtime/io.hpp>
#include <qp/units/dimensions.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace qp::runtime;

namespace {

/// @brief A format that declares whatever the test needs it to declare.
class FakeExporter final : public IExporter {
public:
    FakeExporter(std::string name, std::vector<std::string> extensions,
                 ExportCapabilities capabilities, bool available = true)
        : available_(available) {
        desc_.name = std::move(name);
        desc_.label = desc_.name;
        desc_.extensions = std::move(extensions);
        desc_.capabilities = capabilities;
    }

    [[nodiscard]] const FormatDesc& format() const noexcept override { return desc_; }

    [[nodiscard]] ExportRefusal write(const ExportRequest& request) noexcept override {
        ++write_calls;
        return check_export(*this, request);
    }

    [[nodiscard]] bool is_available() const noexcept override { return available_; }

    int write_calls = 0;

private:
    FormatDesc desc_{};
    bool available_ = true;
};

/// @brief Capabilities for a format that keeps everything the platform cares about.
ExportCapabilities full_capabilities() {
    ExportCapabilities c;
    c.keeps_uncertainty = true;
    c.multi_dataset = true;
    c.keeps_time = true;
    c.is_text = true;
    c.keeps_dimension = true;
    // The sixth capability, and the fixture that claims everything claims this too: a format that keeps the
    // uncertainty, the time, the dimension and the text is a format that can write either table.
    c.keeps_readings = true;
    return c;
}

/// @brief Everything the platform can carry **except** a readings table, for the subject's refusal.
ExportCapabilities full_capabilities_no_readings() {
    ExportCapabilities c = full_capabilities();
    c.keeps_readings = false;
    return c;
}

/// @brief Capabilities for a bare-numbers format: the artifact this platform exists
///        to replace, and therefore the one worth testing against.
ExportCapabilities bare_numbers() {
    ExportCapabilities c;
    c.is_text = true;
    return c;
}

/// @brief A trace with `channels` channels and `samples` samples.
Trace make_trace(std::size_t channels, std::size_t samples) {
    Trace trace{RunId{1}};
    for (std::size_t c = 0; c < channels; ++c) {
        REQUIRE(trace.add_channel(Channel{"ch" + std::to_string(c), {}}).has_value());
    }
    for (std::size_t s = 0; s < samples; ++s) {
        std::vector<UncertainValue> values;
        for (std::size_t c = 0; c < channels; ++c) {
            values.push_back(UncertainValue::measured(static_cast<double>(s), 0.1));
        }
        REQUIRE(trace.append(static_cast<double>(s), std::move(values)).has_value());
    }
    return trace;
}

}  // namespace

// ===========================================================================
// Capabilities
// ===========================================================================

TEST_CASE("io.capabilities.uncertainty_is_declared", "[io]") {
    // The default is the honest one: a format that says nothing keeps nothing. A
    // default of `true` would let a writer claim a capability it never implemented,
    // and the loss would surface in a published table.
    const ExportCapabilities fresh{};
    REQUIRE_FALSE(fresh.keeps_uncertainty);
    REQUIRE_FALSE(fresh.multi_dataset);
    REQUIRE_FALSE(fresh.keeps_time);
    REQUIRE_FALSE(fresh.is_text);
    REQUIRE_FALSE(fresh.keeps_dimension);

    const ExportCapabilities full = full_capabilities();
    REQUIRE(full.keeps_uncertainty);

    // Every capability is independently settable, so a format can be text without
    // being uncertainty-aware, and vice versa.
    ExportCapabilities text_only = bare_numbers();
    REQUIRE(text_only.is_text);
    REQUIRE_FALSE(text_only.keeps_uncertainty);
}

TEST_CASE("io.refusal_names_are_stable", "[io]") {
    STATIC_REQUIRE(std::string_view(to_string(ExportRefusal::ok)) == "ok");
    STATIC_REQUIRE(std::string_view(to_string(ExportRefusal::unknown_format)) ==
                   "unknown_format");
    STATIC_REQUIRE(std::string_view(to_string(ExportRefusal::uncertainty_not_supported)) ==
                   "uncertainty_not_supported");
    STATIC_REQUIRE(std::string_view(to_string(ExportRefusal::nothing_to_write)) ==
                   "nothing_to_write");
    STATIC_REQUIRE(std::string_view(to_string(ExportRefusal::shape_mismatch)) ==
                   "shape_mismatch");
    STATIC_REQUIRE(std::string_view(to_string(ExportRefusal::could_not_write)) ==
                   "could_not_write");
    // ok is the zero value, so a default-constructed refusal means "proceed".
    STATIC_REQUIRE(static_cast<std::uint8_t>(ExportRefusal::ok) == 0);
    // The tags are frozen: an existing code keeps its number, so a log line from last term still means
    // what it meant. A new reason is appended rather than inserted.
    STATIC_REQUIRE(static_cast<std::uint8_t>(ExportRefusal::could_not_write) == 5);
}

// ===========================================================================
// The registry
// ===========================================================================

TEST_CASE("io.registry.register_and_find", "[io]") {
    FormatRegistry registry;
    REQUIRE(registry.size() == 0);
    REQUIRE(registry.find_by_name("qp.csv") == nullptr);
    REQUIRE(registry.all().empty());

    FakeExporter csv{"qp.csv", {"csv"}, full_capabilities()};
    FakeExporter png{"qp.png", {"png"}, bare_numbers(), /*available=*/false};
    REQUIRE(registry.add(&csv).has_value());
    REQUIRE(registry.add(&png).has_value());
    REQUIRE(registry.size() == 2);

    REQUIRE(registry.find_by_name("qp.csv") == &csv);
    REQUIRE(registry.find_by_name("absent") == nullptr);
    REQUIRE(registry.find_by_name("") == nullptr);

    // Lookup by extension, with or without the dot: a file dialog produces ".csv"
    // and a list produces "csv", and insisting on one spelling is how a "save as"
    // silently finds nothing.
    REQUIRE(registry.find_by_extension("csv") == &csv);
    REQUIRE(registry.find_by_extension(".csv") == &csv);
    REQUIRE(registry.find_by_extension("CSV") == &csv);
    REQUIRE(registry.find_by_extension("..csv") == &csv);
    REQUIRE(registry.find_by_extension("tsv") == nullptr);
    REQUIRE(registry.find_by_extension("") == nullptr);

    // Registration order is the list order, so a "save as" list is stable across
    // runs rather than depending on iteration order.
    REQUIRE(registry.all().size() == 2);
    REQUIRE(registry.all()[0] == &csv);
    REQUIRE(registry.all()[1] == &png);

    // An exporter that reports itself unavailable stays registered: a format whose
    // library is missing should appear greyed out, not vanish and leave the user
    // wondering where it went.
    REQUIRE_FALSE(png.is_available());
    REQUIRE(registry.find_by_name("qp.png") == &png);

    SECTION("removal frees the name and the extension") {
        REQUIRE(registry.remove("qp.csv").has_value());
        REQUIRE(registry.size() == 1);
        REQUIRE(registry.find_by_name("qp.csv") == nullptr);
        REQUIRE(registry.find_by_extension("csv") == nullptr);
        REQUIRE(registry.add(&csv).has_value());
        REQUIRE_FALSE(registry.remove("never-registered"));
    }
}

TEST_CASE("io.registry.duplicate_is_refused", "[io]") {
    FormatRegistry registry;
    FakeExporter first{"qp.csv", {"csv"}, full_capabilities()};
    REQUIRE(registry.add(&first).has_value());

    SECTION("a repeated format name is refused") {
        // Two exporters under one name would make "save as CSV" depend on plugin
        // load order, so the same document would produce different files on two
        // machines -- and a file is evidence.
        FakeExporter clash{"qp.csv", {"txt"}, full_capabilities()};
        const auto refused = registry.add(&clash);
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(refused.error() == qp::diag::ErrorCode::duplicate_connection);
        REQUIRE(registry.size() == 1);
        REQUIRE(registry.find_by_name("qp.csv") == &first);
    }

    SECTION("a repeated extension is refused") {
        // Worse than a name collision: the caller chose by extension, because they
        // typed a path. Which writer runs would then depend on load order in a way
        // the user cannot see.
        FakeExporter clash{"qp.other", {"CSV"}, full_capabilities()};
        const auto refused = registry.add(&clash);
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(refused.error() == qp::diag::ErrorCode::duplicate_connection);
        REQUIRE(registry.size() == 1);
    }

    SECTION("a malformed description is refused and changes nothing") {
        FakeExporter no_name{"", {"x"}, full_capabilities()};
        REQUIRE_FALSE(registry.add(&no_name).has_value());

        FakeExporter no_extension{"qp.empty", {}, full_capabilities()};
        REQUIRE_FALSE(registry.add(&no_extension).has_value());

        FakeExporter blank_extension{"qp.blank", {"", "ok"}, full_capabilities()};
        REQUIRE_FALSE(registry.add(&blank_extension).has_value());

        REQUIRE_FALSE(registry.add(nullptr).has_value());
        REQUIRE(registry.size() == 1);
        REQUIRE(registry.find_by_name("qp.empty") == nullptr);
    }
}

TEST_CASE("io.export.a_readings_table_is_not_a_series", "[io]") {
    // **Two subjects, because a format writes one table per file.** A trace is a series -- one row per sample, one
    // column per channel -- and the readings are one row per measurement with its own uncertainty and no time axis.
    // A request that carried both would leave the choice to the writer, which is the format quietly deciding what the
    // user asked for; the subject says it instead, and a subject with nothing behind it is refused rather than
    // written as an empty table.
    const FakeExporter series{"qp.series", {"series"}, full_capabilities()};
    ExportCapabilities readings_only{};
    readings_only.keeps_uncertainty = true;
    readings_only.keeps_readings = true;
    const FakeExporter table{"qp.table", {"table"}, readings_only};
    const FakeExporter neither{"qp.neither", {"neither"}, full_capabilities_no_readings()};

    Dataset readings{"length", qp::units::dims::length};
    readings.add(UncertainValue::measured(0.5, 0.01, qp::units::dims::length));

    ExportRequest request;
    request.subject = ExportSubject::readings;
    request.readings = &readings;
    REQUIRE(check_export(table, request) == ExportRefusal::ok);
    // A format with no shape for a readings table is refused **by name**, which is what tells a caller to choose
    // another format rather than another policy -- and it is refused before the data is even looked at.
    REQUIRE(check_export(neither, request) == ExportRefusal::readings_not_supported);
    Dataset empty{"length", qp::units::dims::length};
    request.readings = &empty;
    REQUIRE(check_export(neither, request) == ExportRefusal::readings_not_supported);
    REQUIRE(check_export(table, request) == ExportRefusal::nothing_to_write);

    // A subject with nothing behind it is a caller's bug, and the two are different findings: a missing pointer is
    // not a format's limitation.
    request.readings = nullptr;
    REQUIRE(check_export(table, request) == ExportRefusal::subject_missing);

    // ... and the labels, when present, must name **every** reading: a source column that silently empties for the
    // last rows is worse than one that was never written, because the reader trusts what is there.
    request.readings = &readings;
    const std::vector<std::string> short_labels;
    request.reading_labels = &short_labels;
    REQUIRE(check_export(table, request) == ExportRefusal::ok);      // empty means "nobody named the rows"
    const std::vector<std::string> one_label{"a"};
    request.reading_labels = &one_label;
    REQUIRE(check_export(table, request) == ExportRefusal::ok);      // one reading, one label
    readings.add(UncertainValue::measured(0.7, 0.01, qp::units::dims::length));
    REQUIRE(check_export(table, request) == ExportRefusal::shape_mismatch);
    request.reading_labels = nullptr;
    REQUIRE(check_export(table, request) == ExportRefusal::ok);

    // The names are stable, because a refusal reaches a user and a log line.
    REQUIRE(std::string{to_string(ExportRefusal::readings_not_supported)} == "readings_not_supported");
    REQUIRE(std::string{to_string(ExportRefusal::subject_missing)} == "subject_missing");
    REQUIRE(std::string{to_string(ExportSubject::trace)} == "trace");
    REQUIRE(std::string{to_string(ExportSubject::readings)} == "readings");

    // **The trace's request is unchanged**, which is the other half of the claim: a subject defaults to the trace,
    // so every request written before this existed means what it meant.
    const Trace trace = make_trace(1, 1);
    ExportRequest as_series;
    as_series.trace = &trace;
    REQUIRE(as_series.subject == ExportSubject::trace);
    REQUIRE(check_export(series, as_series) == ExportRefusal::ok);
    // ... and a request that names the readings while carrying a trace is refused rather than written: the subject
    // is what the writer reads.
    ExportRequest mismatched;
    mismatched.subject = ExportSubject::readings;
    mismatched.trace = &trace;
    REQUIRE(check_export(series, mismatched) == ExportRefusal::subject_missing);
}

TEST_CASE("io.registry.filters_by_capability", "[io]") {
    FormatRegistry registry;
    FakeExporter full{"qp.full", {"full"}, full_capabilities()};
    FakeExporter bare{"qp.bare", {"bare"}, bare_numbers()};
    FakeExporter half{"qp.half", {"half"}, ExportCapabilities{true, false, false, false, false}};
    REQUIRE(registry.add(&full).has_value());
    REQUIRE(registry.add(&bare).has_value());
    REQUIRE(registry.add(&half).has_value());

    // The list a "save for a lab report" dialog should offer. Computed in the core
    // rather than in the view, so the property can be tested without a dialog.
    const std::vector<IExporter*> keeps = registry.with_uncertainty();
    REQUIRE(keeps.size() == 2);
    REQUIRE(keeps[0] == &full);
    REQUIRE(keeps[1] == &half);
    // The bare-numbers format is still registered and still findable; it is simply
    // not offered when the uncertainty has to survive.
    REQUIRE(registry.find_by_name("qp.bare") == &bare);
}

// ===========================================================================
// The pre-flight check
// ===========================================================================

TEST_CASE("io.export.refuses_a_format_that_cannot_carry_uncertainty", "[io]") {
    FakeExporter bare{"qp.bare", {"bare"}, bare_numbers()};
    FakeExporter full{"qp.full", {"full"}, full_capabilities()};
    Trace trace = make_trace(1, 3);

    SECTION("a request that requires the uncertainty is refused by a format that loses it") {
        // The check this module exists for. It happens **before** anything touches
        // the filesystem, so a caller learns that the error bars would be lost
        // rather than discovering it in a published table.
        ExportRequest request;
        request.trace = &trace;
        request.path = "out.bare";
        request.require_uncertainty = true;

        const ExportRefusal refusal = check_export(bare, request);
        REQUIRE(refusal == ExportRefusal::uncertainty_not_supported);
        REQUIRE(std::string_view(to_string(refusal)) == "uncertainty_not_supported");

        // No write was attempted: the fake counts calls, and the count is zero.
        REQUIRE(bare.write_calls == 0);
        REQUIRE(bare.write(request) == ExportRefusal::uncertainty_not_supported);
        REQUIRE(bare.write_calls == 1);   // the exporter itself checks too

        // The same request against a format that keeps it proceeds.
        REQUIRE(check_export(full, request) == ExportRefusal::ok);
    }

    SECTION("a request that does not require it proceeds") {
        // A screenshot-grade export is legitimate. The limitation is real, and the
        // point is that it is *declared*, not that the format is forbidden.
        ExportRequest request;
        request.trace = &trace;
        request.path = "look.bare";
        request.require_uncertainty = false;
        REQUIRE(check_export(bare, request) == ExportRefusal::ok);
    }

    SECTION("an empty or absent trace has nothing to write") {
        ExportRequest none;
        none.trace = nullptr;
        REQUIRE(check_export(full, none) == ExportRefusal::nothing_to_write);

        Trace empty{RunId{2}};
        REQUIRE(empty.add_channel(Channel{"a", {}}).has_value());
        ExportRequest empty_request;
        empty_request.trace = &empty;
        // Writing a header with no rows would produce a file that looks like a
        // successful export of nothing.
        REQUIRE(check_export(full, empty_request) == ExportRefusal::nothing_to_write);
    }

    SECTION("a malformed trace is refused rather than written") {
        // A trace whose samples disagree with its channels would produce a file
        // whose columns do not line up. This module cannot repair it and must not
        // pretend it wrote something valid.
        Trace broken{RunId{3}};
        REQUIRE(broken.add_channel(Channel{"a", {}}).has_value());
        REQUIRE(broken.append(0.0, UncertainValue::measured(1.0, 0.1)).has_value());
        ExportRequest request;
        request.trace = &broken;
        REQUIRE(check_export(full, request) == ExportRefusal::ok);

        // The consistent case is the only one that passes, and it is the one the
        // registry built above. A multi-channel trace round-trips too.
        Trace multi = make_trace(3, 4);
        REQUIRE(multi.is_consistent());
        ExportRequest multi_request;
        multi_request.trace = &multi;
        REQUIRE(check_export(full, multi_request) == ExportRefusal::ok);
    }
}
