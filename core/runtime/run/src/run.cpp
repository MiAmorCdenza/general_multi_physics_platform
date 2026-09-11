/**
 * @file run.cpp
 * @brief Implementation of run identity and the run ledger.
 */
#include <qp/runtime/run/run.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace qp::runtime {
namespace {

/// @brief The six reproducibility inputs, in the order a report should list them.
constexpr ReproField kOrderedFields[] = {
    ReproField::seed,          ReproField::graph_version,
    ReproField::parameters,    ReproField::plugin_versions,
    ReproField::toolchain,     ReproField::optimisation,
};

}  // namespace

ReproField RunSpec::missing() const noexcept {
    const auto present = static_cast<std::uint32_t>(fields);
    const auto all = static_cast<std::uint32_t>(kAllReproFields);
    return static_cast<ReproField>(all & ~present);
}

std::vector<std::string> RunSpec::missing_names() const noexcept {
    const ReproField gaps = missing();
    std::vector<std::string> out;
    for (const ReproField f : kOrderedFields) {
        if (has_field(gaps, f)) out.emplace_back(to_string(f));
    }
    return out;
}

std::string RunSpec::summary() const noexcept {
    const ReproField gaps = missing();
    std::string out;
    // Fixed shape, so two summaries can be compared as text and a log line stays
    // greppable. No parameters here on purpose: the full set is in the record,
    // and a summary that embedded it would be unreadable.
    out.reserve(96);
    out += "run(seed=";
    out += std::to_string(seed);
    out += ", graph=v";
    out += std::to_string(graph_version);
    out += ", plugins=";
    out += std::to_string(plugins.size());
    out += ", params=";
    out += std::to_string(parameters.size());
    out += ", bit_exact=";
    out += bit_exact ? "yes" : "no";
    if (gaps == ReproField::none) {
        out += ", complete)";
        return out;
    }
    out += ", MISSING=";
    bool first = true;
    for (const ReproField f : kOrderedFields) {
        if (!has_field(gaps, f)) continue;
        if (!first) out += '+';
        out += to_string(f);
        first = false;
    }
    out += ')';
    return out;
}

std::string_view RunSpec::parameter(std::string_view name) const noexcept {
    for (const RecordedParameter& p : parameters) {
        if (p.name == name) return p.value;
    }
    return {};
}

RunId RunLedger::begin(RunSpec spec) noexcept {
    RunRecord record;
    record.id = RunId{next_id_++};
    record.spec = std::move(spec);
    records_.push_back(std::move(record));
    return records_.back().id;
}

const RunRecord* RunLedger::find(RunId id) const noexcept {
    if (!id.valid()) return nullptr;
    for (const RunRecord& r : records_) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

RunId RunLedger::last_id() const noexcept {
    return records_.empty() ? RunId{} : records_.back().id;
}

std::size_t RunLedger::incomplete_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(records_.begin(), records_.end(),
                      [](const RunRecord& r) { return !r.is_complete(); }));
}

const char* toolchain_id() noexcept {
    // A function-local static string built once: the answer cannot change during
    // the process, and formatting it per call would put an allocation on a path
    // a caller may hit per run.
    static const std::string id = [] {
        char buf[64];
#if defined(_MSC_VER)
        std::snprintf(buf, sizeof(buf), "msvc-%d.%d", _MSC_VER / 100, _MSC_VER % 100);
#elif defined(__clang__)
        std::snprintf(buf, sizeof(buf), "clang-%d.%d.%d", __clang_major__, __clang_minor__,
                      __clang_patchlevel__);
#elif defined(__GNUC__)
        std::snprintf(buf, sizeof(buf), "gcc-%d.%d.%d", __GNUC__, __GNUC_MINOR__,
                      __GNUC_PATCHLEVEL__);
#else
        std::snprintf(buf, sizeof(buf), "unknown");
#endif
        return std::string{buf};
    }();
    return id.c_str();
}

const char* optimisation_id() noexcept {
    // Reports only what the macros actually establish. `__OPTIMIZE__` is defined
    // whenever optimisation is on, but says nothing about the level, so a record
    // that printed "-O2" here would be inventing a detail it cannot see -- and an
    // invented detail in a reproducibility record is worse than an absent one.
#if defined(NDEBUG)
    return "release";
#elif defined(__OPTIMIZE__) || defined(_MSC_VER)
    return "release";
#else
    return "debug";
#endif
}

std::int64_t now_unix_seconds() noexcept {
    using namespace std::chrono;
    const auto since_epoch = system_clock::now().time_since_epoch();
    return static_cast<std::int64_t>(duration_cast<seconds>(since_epoch).count());
}

}  // namespace qp::runtime
