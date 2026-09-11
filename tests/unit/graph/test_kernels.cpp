/**
 * @file test_kernels.cpp
 * @brief Tests for the native operator contract and its registry.
 *
 * Test case ids match the @tests fields in the kernels headers byte for byte.
 *
 * The test kernels below are deliberately trivial and deliberately not physics:
 * this module must not contain an integrator, and a test that shipped a
 * half-correct Boris push would blur exactly the boundary the module exists to
 * hold. What is under test is the contract -- registration states, batch
 * validation, scratch requirements, and reproducibility of the injected RNG.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/kernels.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

using namespace qp::graph;
using namespace qp::graph::kernels;

namespace {

/// @brief A test operator that doubles every value in place.
///
/// Marked in-place: it reads and writes the same buffer, so the host must not
/// hand it buffers it does not own.
class DoubleAdvancer final : public IBatchAdvancer {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "test.double"; }
    [[nodiscard]] Capability capabilities() const noexcept override {
        return Capability::is_in_place;
    }
    [[nodiscard]] qp::diag::Result<void> prepare(const ParamBlock&) override {
        ++prepare_calls;
        return {};
    }
    [[nodiscard]] qp::diag::Result<void> advance(const BatchView& batch,
                                             AdvanceContext& ctx) override {
        if (!batch.valid()) {
            return qp::diag::ErrorCode::invalid_argument;
        }
        ++steps;
        last_dt = ctx.dt;
        last_step = ctx.step;
        for (std::size_t i = 0; i < batch.count; ++i) {
            auto& v = batch.in[i];
            const std::uint64_t points = v.point_count();
            auto* data = static_cast<float*>(const_cast<void*>(v.data));
            if (data == nullptr) continue;
            const std::uint32_t components = field::component_count(v.components());
            for (std::uint64_t k = 0; k < points * components; ++k) {
                data[k] = data[k] * 2.0F;
            }
        }
        return {};
    }

    int prepare_calls = 0;
    int steps = 0;
    double last_dt = 0.0;
    std::uint64_t last_step = 0;
};

/// @brief An operator that requires scratch space, and says so.
class ScratchAdvancer final : public IBatchAdvancer {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "test.scratch"; }
    [[nodiscard]] Capability capabilities() const noexcept override {
        return Capability::needs_scratch | Capability::is_stochastic;
    }
    [[nodiscard]] qp::diag::Result<void> prepare(const ParamBlock&) override { return {}; }
    [[nodiscard]] qp::diag::Result<void> advance(const BatchView& batch,
                                             AdvanceContext& ctx) override {
        if (!batch.valid()) {
            return qp::diag::ErrorCode::invalid_argument;
        }
        // An operator that declared needs_scratch must be given scratch. The
        // host promises it; a kernel that silently copes would hide a host bug.
        if (ctx.scratch == nullptr || ctx.scratch_bytes == 0) {
            return qp::diag::ErrorCode::invalid_argument;
        }
        for (std::size_t i = 0; i < batch.count; ++i) {
            (void)ctx.next_unit();
        }
        ++steps;
        return {};
    }

    int steps = 0;
};

/// @brief Collects the first `n` values the injected RNG produces.
std::vector<double> sample_rng(std::uint64_t seed, int n) {
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(n));
    std::uint64_t state = seed;
    AdvanceContext ctx;
    ctx.seed = seed;
    ctx.rng = &state;
    for (int i = 0; i < n; ++i) out.push_back(ctx.next_unit());
    return out;
}

}  // namespace

// ===========================================================================
// Capabilities and the parameter block
// ===========================================================================

TEST_CASE("kernel.capabilities.flags", "[kernel]") {
    STATIC_REQUIRE(!has_capability(Capability::none, Capability::is_in_place));
    STATIC_REQUIRE(has_capability(Capability::is_in_place, Capability::is_in_place));
    STATIC_REQUIRE(has_capability(Capability::needs_scratch, Capability::needs_scratch));
    STATIC_REQUIRE(has_capability(Capability::is_stochastic, Capability::is_stochastic));
    STATIC_REQUIRE(has_capability(Capability::is_neighbourhood, Capability::is_neighbourhood));

    // The flags are independent bits, so a combination must keep every one of
    // them: an operator that is both stochastic and in-place must report both, or
    // the host will skip reserving a private buffer for it.
    const Capability both = Capability::is_in_place | Capability::is_stochastic;
    REQUIRE(has_capability(both, Capability::is_in_place));
    REQUIRE(has_capability(both, Capability::is_stochastic));
    REQUIRE_FALSE(has_capability(both, Capability::needs_scratch));
}

TEST_CASE("kernel.param_block.slots", "[kernel]") {
    ParamBlock p;
    // Unused slots stay zero: an operator that reads a slot the host never wrote
    // must see 0, not whatever was on the stack.
    for (std::size_t i = 0; i < ParamBlock::kDoubles; ++i) REQUIRE(p.real(i) == 0.0);
    for (std::size_t i = 0; i < ParamBlock::kIntegers; ++i) REQUIRE(p.integer(i) == 0);

    p.set_real(0, 1.5);
    p.set_integer(3, -7);
    REQUIRE(p.real(0) == 1.5);
    REQUIRE(p.integer(3) == -7);

    // Out-of-range access is defined, not undefined: it returns the type's zero
    // and the write is ignored.
    REQUIRE(p.real(ParamBlock::kDoubles) == 0.0);
    REQUIRE(p.integer(ParamBlock::kIntegers) == 0);
    p.set_real(ParamBlock::kDoubles + 10, 99.0);
    p.set_integer(ParamBlock::kIntegers + 10, 99);
    REQUIRE(p.real(ParamBlock::kDoubles + 10) == 0.0);
    REQUIRE(p.integer(ParamBlock::kIntegers + 10) == 0);
}

TEST_CASE("kernel.batch_view.span", "[kernel]") {
    const BatchView empty{};
    REQUIRE_FALSE(empty.valid());

    std::vector<float> storage{1.0F, 2.0F};
    field::FieldValue v;
    v.desc = qp::abi::make_lattice(qp::abi::LatticeKind::line, qp::abi::ComponentKind::scalar,
                                   qp::abi::ElementType::f32, qp::abi::FieldDim{1, 0, 0, 0, 0, 0, 0}, 2);
    v.data = storage.data();
    v.bytes = storage.size() * sizeof(float);

    BatchView one;
    one.in = &v;
    one.out = &v;
    one.count = 1;
    REQUIRE(one.valid());

    // A null array or a zero count is not a batch, whatever else is set. A count
    // of zero in particular must be rejected rather than treated as "nothing to
    // do", because a host that passes zero has a bug and would loop forever.
    BatchView no_in = one;
    no_in.in = nullptr;
    REQUIRE_FALSE(no_in.valid());

    BatchView no_out = one;
    no_out.out = nullptr;
    REQUIRE_FALSE(no_out.valid());

    BatchView zero = one;
    zero.count = 0;
    REQUIRE_FALSE(zero.valid());
}

// ===========================================================================
// Registry
// ===========================================================================

TEST_CASE("kernel.registry.register_and_find", "[kernel]") {
    DoubleAdvancer impl;
    KernelRegistry reg;

    REQUIRE(reg.size() == 0);
    REQUIRE(reg.reserved_count() == 0);

    auto added = reg.add(KernelDesc{"boris", "Boris push", Capability::is_in_place, &impl});
    REQUIRE(added.has_value());
    const KernelId id = added.value();
    REQUIRE(id.valid());
    REQUIRE(reg.size() == 1);

    const KernelDesc* found = reg.find(id);
    REQUIRE(found != nullptr);
    REQUIRE(found->name == "boris");
    REQUIRE(found->summary == "Boris push");
    REQUIRE(found->impl == &impl);
    REQUIRE(has_capability(found->capabilities, Capability::is_in_place));

    // Lookup by name returns the same entry, and by a dead id returns nothing.
    REQUIRE(reg.find_by_name("boris") == found);
    REQUIRE(reg.find_by_name("nope") == nullptr);
    REQUIRE(reg.find(kNoKernel) == nullptr);
    REQUIRE(reg.find(KernelId{999}) == nullptr);
    REQUIRE_FALSE(reg.is_live(KernelId{999}));
    REQUIRE(reg.is_live(id));
}

TEST_CASE("kernel.registry.rejects_bad_input", "[kernel]") {
    DoubleAdvancer impl;
    KernelRegistry reg;

    SECTION("empty name") {
        const auto r = reg.add(KernelDesc{"", "no name", Capability::none, &impl});
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error() == qp::diag::ErrorCode::invalid_argument);
        REQUIRE(reg.size() == 0);
        REQUIRE(reg.reserved_count() == 0);   // and no reservation is left behind
    }

    SECTION("null implementation") {
        const auto r = reg.add(KernelDesc{"boris", "", Capability::none, nullptr});
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error() == qp::diag::ErrorCode::invalid_argument);
        REQUIRE(reg.reserved_count() == 0);
    }

    SECTION("duplicate name") {
        REQUIRE(reg.add(KernelDesc{"boris", "", Capability::none, &impl}).has_value());

        DoubleAdvancer other;
        const auto r = reg.add(KernelDesc{"boris", "", Capability::none, &other});
        REQUIRE_FALSE(r.has_value());
        // duplicate_connection, not invalid_argument: the argument is well formed,
        // it is the state that refuses it. A caller that retries on
        // invalid_argument must not retry this one.
        REQUIRE(r.error() == qp::diag::ErrorCode::duplicate_connection);

        // The original is untouched: overwriting would silently change the
        // physics of every saved graph that names this kernel.
        REQUIRE(reg.size() == 1);
        REQUIRE(reg.find_by_name("boris")->impl == &impl);
    }
}

TEST_CASE("kernel.registry.declare_commit_drop", "[kernel]") {
    DoubleAdvancer impl;
    KernelRegistry reg;

    // Declaring reserves but does not expose: an entry that is visible before its
    // implementation is ready is one the editor could wire into a graph.
    auto declared = reg.declare("boris");
    REQUIRE(declared.has_value());
    const KernelId id = declared.value();
    REQUIRE(id.valid());
    REQUIRE(reg.size() == 0);              // not live
    REQUIRE(reg.reserved_count() == 1);    // but reserved
    REQUIRE(reg.find(id) == nullptr);
    REQUIRE(reg.find_by_name("boris") == nullptr);
    REQUIRE_FALSE(reg.is_live(id));
    // The name is taken even while reserved, so a second plugin cannot claim it.
    REQUIRE_FALSE(reg.declare("boris").has_value());

    // Committing with the wrong name is refused: the name is the identity a
    // saved document refers to.
    const auto mismatched = reg.commit(id, KernelDesc{"leapfrog", "", Capability::none, &impl});
    REQUIRE_FALSE(mismatched.has_value());
    REQUIRE(mismatched.error() == qp::diag::ErrorCode::plugin_incompatible);
    REQUIRE_FALSE(reg.is_live(id));

    REQUIRE(reg.commit(id, KernelDesc{"boris", "", Capability::none, &impl}).has_value());
    REQUIRE(reg.is_live(id));
    REQUIRE(reg.size() == 1);
    REQUIRE(reg.reserved_count() == 0);

    // Committing twice is refused rather than silently replacing.
    DoubleAdvancer other;
    REQUIRE_FALSE(reg.commit(id, KernelDesc{"boris", "", Capability::none, &other}).has_value());

    // Dropping frees both the id and the name.
    REQUIRE(reg.drop(id).has_value());
    REQUIRE(reg.size() == 0);
    REQUIRE(reg.find(id) == nullptr);
    REQUIRE(reg.find_by_name("boris") == nullptr);
    REQUIRE(reg.declare("boris").has_value());

    // Uncommitted and unknown ids are not droppable through a stale handle.
    REQUIRE_FALSE(reg.drop(KernelId{999}).has_value());
    REQUIRE_FALSE(reg.commit(KernelId{999}, KernelDesc{"x", "", Capability::none, &impl})
                      .has_value());
}

TEST_CASE("kernel.registry.failed_load_leaves_no_trace", "[kernel]") {
    // The defect this models: a plugin announces a kernel, then fails to load,
    // and the kernel stays behind. It shows up in the editor, users wire it in,
    // and the run fails -- which is indistinguishable from a physics bug.
    DoubleAdvancer impl;
    KernelRegistry reg;

    const auto before = reg.size();
    const auto reserved_before = reg.reserved_count();

    // A plugin that gets as far as declaring, then fails.
    auto declared = reg.declare("boris");
    REQUIRE(declared.has_value());

    // Its cleanup path drops the reservation. Whatever the plugin does after
    // this, the registry must look exactly as it did before.
    REQUIRE(reg.drop(declared.value()).has_value());
    REQUIRE(reg.size() == before);
    REQUIRE(reg.reserved_count() == reserved_before);
    REQUIRE(reg.find_by_name("boris") == nullptr);

    // A failed add() must clean up after itself too, with no explicit drop.
    REQUIRE_FALSE(reg.add(KernelDesc{"", "", Capability::none, &impl}).has_value());
    REQUIRE(reg.size() == before);
    REQUIRE(reg.reserved_count() == reserved_before);

    // And the module ships no kernel of its own: an empty registry stays empty.
    REQUIRE(reg.list().empty());
}

TEST_CASE("kernel.registry.listing_is_reservation_ordered", "[kernel]") {
    DoubleAdvancer a, b, c;
    KernelRegistry reg;

    auto first = reg.add(KernelDesc{"first", "1", Capability::none, &a});
    auto second = reg.add(KernelDesc{"second", "2", Capability::none, &b});
    auto third = reg.add(KernelDesc{"third", "3", Capability::none, &c});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());

    const std::vector<KernelDesc> listed = reg.list();
    REQUIRE(listed.size() == 3);
    // Registration order, not sorted: sorting would be a second ordering rule
    // that the user could not predict from the order their plugins loaded.
    REQUIRE(listed[0].name == "first");
    REQUIRE(listed[1].name == "second");
    REQUIRE(listed[2].name == "third");

    // Dropping from the middle disturbs nothing else: the surviving two keep
    // their relative order, and the dead name becomes available again.
    REQUIRE(reg.drop(second.value()).has_value());
    const std::vector<KernelDesc> after = reg.list();
    REQUIRE(after.size() == 2);
    REQUIRE(after[0].name == "first");
    REQUIRE(after[1].name == "third");
    REQUIRE(reg.find(second.value()) == nullptr);
    REQUIRE(reg.find(first.value()) != nullptr);
    REQUIRE(reg.find(third.value()) != nullptr);
    REQUIRE(reg.declare("second").has_value());
}

// ===========================================================================
// Advance and determinism
// ===========================================================================

TEST_CASE("kernel.advance.rejects_bad_batch", "[kernel]") {
    DoubleAdvancer impl;
    AdvanceContext ctx;
    const BatchView bad{};

    const auto r = impl.advance(bad, ctx);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == qp::diag::ErrorCode::invalid_argument);
    REQUIRE(impl.steps == 0);

    // A valid batch runs, and the context reaches the operator intact: dt and
    // step come from the host, not from a clock the kernel reads itself.
    std::vector<float> storage{1.0F, 2.0F};
    field::FieldValue v;
    v.desc = qp::abi::make_lattice(qp::abi::LatticeKind::line, qp::abi::ComponentKind::scalar,
                                   qp::abi::ElementType::f32, qp::abi::FieldDim{1, 0, 0, 0, 0, 0, 0}, 2);
    v.data = storage.data();
    v.bytes = storage.size() * sizeof(float);

    BatchView good;
    good.in = &v;
    good.out = &v;
    good.count = 1;
    ctx.dt = 0.25;
    ctx.step = 7;
    REQUIRE(impl.advance(good, ctx).has_value());
    REQUIRE(impl.steps == 1);
    REQUIRE(impl.last_dt == 0.25);
    REQUIRE(impl.last_step == 7);
    REQUIRE(storage[0] == 2.0F);
    REQUIRE(storage[1] == 4.0F);
}

TEST_CASE("kernel.prepare.rejects_without_scratch", "[kernel]") {
    ScratchAdvancer impl;
    KernelRegistry reg;
    REQUIRE(reg.add(KernelDesc{"stochastic", "", impl.capabilities(), &impl}).has_value());

    // prepare runs at load time, once, and is where a plugin validates its
    // parameters -- while the user is still editing rather than mid-run.
    const ParamBlock params;
    REQUIRE(impl.prepare(params).has_value());

    // The capability tells the host a scratch buffer is mandatory. An operator
    // that demands scratch and is given none must refuse rather than proceed and
    // read past a null pointer.
    std::vector<float> storage{1.0F};
    field::FieldValue v;
    v.desc = qp::abi::make_lattice(qp::abi::LatticeKind::line, qp::abi::ComponentKind::scalar,
                                   qp::abi::ElementType::f32, qp::abi::FieldDim{1, 0, 0, 0, 0, 0, 0}, 1);
    v.data = storage.data();
    v.bytes = storage.size() * sizeof(float);
    BatchView batch;
    batch.in = &v;
    batch.out = &v;
    batch.count = 1;

    AdvanceContext no_scratch;
    const auto refused = impl.advance(batch, no_scratch);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error() == qp::diag::ErrorCode::invalid_argument);
    REQUIRE(impl.steps == 0);

    // With scratch it runs.
    std::uint64_t scratch_word = 0;
    AdvanceContext with_scratch;
    with_scratch.scratch = &scratch_word;
    with_scratch.scratch_bytes = sizeof(scratch_word);
    REQUIRE(impl.advance(batch, with_scratch).has_value());
    REQUIRE(impl.steps == 1);
}

TEST_CASE("kernel.determinism.same_seed_same_stream", "[kernel]") {
    // Charter R2: the same seed must reproduce the same run bit for bit. The
    // RNG the contract hands out therefore cannot be a standard distribution
    // adaptor (libstdc++ and MSVC produce different sequences from the same
    // engine), so next_unit() is a splitmix64 step: identical everywhere.
    const std::vector<double> a = sample_rng(20260911ULL, 8);
    const std::vector<double> b = sample_rng(20260911ULL, 8);
    REQUIRE(a == b);

    // A different seed must produce a different stream, or "reproducible" would
    // be satisfied by ignoring the seed entirely.
    const std::vector<double> c = sample_rng(20260912ULL, 8);
    REQUIRE(a != c);

    for (const double x : a) {
        REQUIRE(x >= 0.0);
        REQUIRE(x < 1.0);
    }

    // The values are spread out, not clustered: a degenerate generator that
    // always returns 0 would pass every check above.
    REQUIRE(a[0] != a[1]);
    double sum = 0.0;
    for (const double x : a) sum += x;
    REQUIRE(sum > 0.5);
    REQUIRE(sum < 7.5);

    // With no RNG the contract yields 0.0 rather than reading through a null
    // pointer. A deterministic operator never sees the difference.
    AdvanceContext none;
    REQUIRE(none.rng == nullptr);
    REQUIRE(none.next_unit() == 0.0);
}

TEST_CASE("kernel.prepare.counts_calls", "[kernel]") {
    // prepare is a load-time hook. Running it twice with equal parameters must be
    // harmless, so an operator is free to be called again after a hot reload.
    DoubleAdvancer impl;
    const ParamBlock params;
    REQUIRE(impl.prepare(params).has_value());
    REQUIRE(impl.prepare(params).has_value());
    REQUIRE(impl.prepare_calls == 2);
}
