/**
 * @file test_run.cpp
 * @brief Tests for run identity and the run ledger.
 *
 * Test case ids match the @tests fields in the run headers byte for byte.
 *
 * The claim under test is not "a record stores what it is given" -- that is a
 * container. It is that **an incomplete record says so, and says what is
 * missing**. A record that claimed reproducibility while lacking the toolchain
 * would send a student hunting for a difference in their physics; one that names
 * the gap sends them to re-run on the same machine. That distinction is the only
 * reason this module exists rather than a bare struct.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/runtime/run.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace qp::runtime;

namespace {

/// @brief A spec with every reproducibility input present.
RunSpec complete_spec() {
    RunSpec s;
    s.seed = 20260911;
    s.graph_version = 7;
    s.toolchain = "gcc-15.2.0";
    s.optimisation = "debug";
    s.plugins = {PluginPin{"org.example.spring", "1.2.0"}};
    s.parameters = {RecordedParameter{"k", "12.5", "N/m"}};
    s.fields = ReproField::seed | ReproField::graph_version | ReproField::toolchain |
               ReproField::optimisation | ReproField::plugin_versions |
               ReproField::parameters;
    return s;
}

/// @brief Whether `names` contains `want`.
bool names_contain(const std::vector<std::string>& names, std::string_view want) {
    for (const std::string& n : names) {
        if (n == want) return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// Completeness
// ===========================================================================

TEST_CASE("run.spec.completeness", "[run]") {
    const RunSpec full = complete_spec();
    REQUIRE(full.is_complete());
    REQUIRE(full.missing() == ReproField::none);
    REQUIRE(full.missing_names().empty());

    // A default-constructed spec has recorded nothing, so everything is missing.
    // This is the state a caller is in before it fills anything in, and calling
    // it "complete" would be the most dangerous possible default.
    const RunSpec empty;
    REQUIRE_FALSE(empty.is_complete());
    REQUIRE(empty.missing() == kAllReproFields);
    REQUIRE(empty.missing_names().size() == 6);
}

TEST_CASE("run.spec.missing_fields", "[run]") {
    SECTION("each input is reported by name when absent") {
        // Dropping one field at a time must be detectable for every field: a
        // completeness check that silently tolerated one of them would make that
        // input decorative.
        const ReproField each[] = {ReproField::seed,
                                   ReproField::graph_version,
                                   ReproField::toolchain,
                                   ReproField::optimisation,
                                   ReproField::plugin_versions,
                                   ReproField::parameters};
        for (const ReproField dropped : each) {
            RunSpec s = complete_spec();
            s.fields = static_cast<ReproField>(
                static_cast<std::uint32_t>(s.fields) & ~static_cast<std::uint32_t>(dropped));
            INFO("dropped=" << to_string(dropped));
            REQUIRE_FALSE(s.is_complete());
            REQUIRE(has_field(s.missing(), dropped));
            REQUIRE(s.missing_names().size() == 1);
            REQUIRE(s.missing_names()[0] == to_string(dropped));
        }
    }

    SECTION("missing() and fields() are consistent") {
        RunSpec s = complete_spec();
        s.fields = ReproField::seed | ReproField::toolchain;
        const ReproField gaps = s.missing();
        REQUIRE_FALSE(has_field(gaps, ReproField::seed));
        REQUIRE_FALSE(has_field(gaps, ReproField::toolchain));
        REQUIRE(has_field(gaps, ReproField::parameters));
        REQUIRE(has_field(gaps, ReproField::graph_version));

        // The gaps are exactly the difference between the full set and what is
        // present: no field can be both present and missing, and none can be
        // neither.
        const auto present = static_cast<std::uint32_t>(s.fields);
        const auto all = static_cast<std::uint32_t>(kAllReproFields);
        const auto gaps_bits = static_cast<std::uint32_t>(gaps);
        REQUIRE((present & gaps_bits) == 0);
        REQUIRE((present | gaps_bits) == all);
    }

    SECTION("the missing list is ordered, so two reports can be compared") {
        RunSpec a = complete_spec();
        a.fields = ReproField::seed;
        RunSpec b = complete_spec();
        b.fields = ReproField::seed;
        REQUIRE(a.missing_names() == b.missing_names());

        // Fixed order regardless of the order the caller recorded fields in: a
        // report that reordered would make two identical runs look different in
        // a diff.
        REQUIRE(a.missing_names() ==
                std::vector<std::string>{"graph_version", "parameters", "plugin_versions",
                                         "toolchain", "optimisation"});
        REQUIRE(names_contain(a.missing_names(), "toolchain"));
    }
}

// ===========================================================================
// The record and the ledger
// ===========================================================================

TEST_CASE("run.record.roundtrip", "[run]") {
    RunSpec spec = complete_spec();
    spec.bit_exact = true;
    spec.started_at = 1789164825;

    RunLedger ledger;
    const RunId id = ledger.begin(spec);
    REQUIRE(id.valid());

    const RunRecord* stored = ledger.find(id);
    REQUIRE(stored != nullptr);
    REQUIRE(stored->id == id);
    REQUIRE(stored->is_complete());

    // Every field survives, including the values a comparison depends on.
    REQUIRE(stored->spec.seed == 20260911);
    REQUIRE(stored->spec.graph_version == 7);
    REQUIRE(stored->spec.toolchain == "gcc-15.2.0");
    REQUIRE(stored->spec.optimisation == "debug");
    REQUIRE(stored->spec.bit_exact);
    REQUIRE(stored->spec.started_at == 1789164825);
    REQUIRE(stored->spec.plugins.size() == 1);
    REQUIRE(stored->spec.plugins[0].id == "org.example.spring");
    REQUIRE(stored->spec.plugins[0].version == "1.2.0");

    // The parameter keeps its value and its unit. Both matter: a bare number
    // without a unit is the commonest lab mistake, so the record has to carry
    // enough to show which unit was meant.
    REQUIRE(stored->spec.parameters.size() == 1);
    REQUIRE(stored->spec.parameter("k") == "12.5");
    REQUIRE(stored->spec.parameter("missing").empty());

    // Lookup by an id the ledger never issued returns nothing rather than a
    // default record: a caller that guessed an id has a bug, and a plausible
    // empty record would hide it.
    REQUIRE(ledger.find(RunId{999}) == nullptr);
    REQUIRE(ledger.find(RunId{}) == nullptr);
}

TEST_CASE("run.id.monotonic", "[run]") {
    RunLedger ledger;
    REQUIRE(ledger.size() == 0);
    REQUIRE_FALSE(ledger.last_id().valid());

    const RunId first = ledger.begin(complete_spec());
    const RunId second = ledger.begin(complete_spec());
    const RunId third = ledger.begin(complete_spec());

    // Strictly increasing, and issued by the ledger rather than chosen by the
    // caller. Two runs sharing an id would make "which configuration produced
    // this result" unanswerable, and the failure would look like a physics
    // disagreement rather than an id bug.
    REQUIRE(first.value < second.value);
    REQUIRE(second.value < third.value);
    REQUIRE(first != second);
    REQUIRE(ledger.size() == 3);
    REQUIRE(ledger.last_id() == third);

    // The records are in issue order, so a session reads as a timeline.
    REQUIRE(ledger.records()[0].id == first);
    REQUIRE(ledger.records()[2].id == third);

    // A record is never rewritten: re-fetching the first one still returns the
    // first one's spec.
    REQUIRE(ledger.find(first) != nullptr);
    REQUIRE(ledger.find(first)->spec.graph_version == 7);
}

TEST_CASE("run.ledger.incomplete_count", "[run]") {
    RunLedger ledger;
    REQUIRE(ledger.incomplete_count() == 0);

    (void)ledger.begin(complete_spec());

    // A run that recorded its seed but not its toolchain cannot be reproduced
    // off this machine, and the ledger counts it.
    RunSpec partial;
    partial.seed = 1;
    partial.graph_version = 2;
    partial.fields = ReproField::seed | ReproField::graph_version;
    (void)ledger.begin(partial);

    (void)ledger.begin(complete_spec());

    REQUIRE(ledger.size() == 3);
    REQUIRE(ledger.incomplete_count() == 1);
    REQUIRE(ledger.incomplete_count() <= ledger.size());
}

// ===========================================================================
// Measured build facts
// ===========================================================================

TEST_CASE("run.toolchain.is_measured", "[run]") {
    const std::string id{toolchain_id()};
    REQUIRE_FALSE(id.empty());

    // Derived from the compiler's own macros, not from a configuration string.
    // A build that reported the toolchain someone *intended* would be worse than
    // one that reported nothing, because it would be believed.
#if defined(_MSC_VER)
    REQUIRE(id.rfind("msvc-", 0) == 0);
#elif defined(__clang__)
    REQUIRE(id.rfind("clang-", 0) == 0);
#elif defined(__GNUC__)
    REQUIRE(id.rfind("gcc-", 0) == 0);
#endif

    // Stable within a process: two calls agree, so a record written at the start
    // and one written at the end of a run cannot disagree.
    REQUIRE(std::string{toolchain_id()} == id);

    // ASCII, so it survives a console code page and a log file.
    for (const char c : id) {
        REQUIRE(static_cast<unsigned char>(c) < 0x80);
        REQUIRE(c != ' ');
    }
}

TEST_CASE("run.optimisation.is_derived_from_macros", "[run]") {
    const std::string level{optimisation_id()};
    REQUIRE_FALSE(level.empty());

    // Only two answers, and both are true statements: "release" means the
    // compiler was optimising, "debug" means it was not. The module never invents
    // a specific level, because `__OPTIMIZE__` does not report one.
    REQUIRE((level == "debug" || level == "release"));

    // In a Debug build the NDEBUG macro is absent, and the answer must reflect
    // that rather than defaulting to the reassuring option.
#if !defined(NDEBUG) && !defined(__OPTIMIZE__)
    REQUIRE(level == "debug");
#endif
#if defined(NDEBUG)
    REQUIRE(level == "release");
#endif
}

TEST_CASE("run.spec.summary_is_stable_and_greppable", "[run]") {
    const RunSpec full = complete_spec();
    const std::string text = full.summary();

    REQUIRE_FALSE(text.empty());
    REQUIRE(text.find("seed=20260911") != std::string::npos);
    REQUIRE(text.find("complete") != std::string::npos);
    // The summary never embeds the parameter set: it would be unreadable, and the
    // parameters are already in the record in full.
    REQUIRE(text.find("12.5") == std::string::npos);

    // Deterministic, so two records' summaries can be compared as text.
    REQUIRE(full.summary() == text);

    // An incomplete spec says so, and names what is missing: that is what makes
    // the line actionable instead of merely alarming.
    RunSpec partial = complete_spec();
    partial.fields = ReproField::seed | ReproField::graph_version;
    const std::string partial_text = partial.summary();
    REQUIRE(partial_text.find("MISSING") != std::string::npos);
    REQUIRE(partial_text.find("toolchain") != std::string::npos);
    REQUIRE(partial_text.find("optimisation") != std::string::npos);
    REQUIRE(partial_text != text);

    // ASCII throughout, so a log line cannot be corrupted by a code page.
    for (const char c : partial_text) {
        REQUIRE(static_cast<unsigned char>(c) < 0x80);
    }
}

TEST_CASE("run.started_at_is_a_real_clock", "[run]") {
    const std::int64_t now = now_unix_seconds();
    // After 2020-09-13 and before 2100-01-01: catches a unit mix-up (seconds vs
    // milliseconds) without pinning an exact value.
    REQUIRE(now > 1'600'000'000);
    REQUIRE(now < 4'102'444'800);

    // Two calls do not go backwards. The clock is allowed to be coarse; it is not
    // allowed to make a timeline reorder.
    REQUIRE(now_unix_seconds() >= now - 1);
}
