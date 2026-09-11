/**
 * @file test_ports.cpp
 * @brief Unit and property tests for the ports module.
 *
 * The case ids correspond verbatim to the @tests fields under core/ports/include.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/ports.hpp>

#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

using namespace qp::ports;
using qp::units::Dim;

// ===========================================================================
// ValueKind / Value
// ===========================================================================

TEST_CASE("ports.value.default_is_invalid", "[ports]") {
    const Value v;
    REQUIRE(v.kind() == ValueKind::invalid);
    REQUIRE_FALSE(v.valid());
    REQUIRE_FALSE(v.is_numeric());
    REQUIRE(std::string(v.kind_name()) == "invalid");
    // Accessors must return a fallback value, never UB
    REQUIRE(v.as_f64() == 0.0);
    REQUIRE(v.as_f32() == 0.0f);
    REQUIRE(v.as_i64() == 0);
    REQUIRE_FALSE(v.as_bool());
    REQUIRE(v.as_text().empty());
    REQUIRE(v.as_dimension() == Dim{});
}

TEST_CASE("ports.value.construction", "[ports]") {
    REQUIRE(Value{1.5}.kind() == ValueKind::f64);
    REQUIRE(Value{1.5f}.kind() == ValueKind::f32);
    REQUIRE(Value{std::int64_t{7}}.kind() == ValueKind::i64);
    REQUIRE(Value{true}.kind() == ValueKind::boolean);
    REQUIRE(Value{std::string{"x"}}.kind() == ValueKind::text);
    REQUIRE(Value{Dim{1, 0, 0, 0, 0, 0, 0}}.kind() == ValueKind::dimension);
    REQUIRE(Value{qp::abi::LatticeDesc{}}.kind() == ValueKind::field_handle);

    // Equality semantics
    REQUIRE(Value{1.5} == Value{1.5});
    REQUIRE(Value{1.5} != Value{2.5});
    REQUIRE(Value{1.5} != Value{1.5f});   // different kinds are never equal
}

TEST_CASE("ports.value.numeric_accessors", "[ports]") {
    const Value d{2.5};
    REQUIRE(d.as_f64() == 2.5);
    REQUIRE(d.is_numeric());
    REQUIRE(d.to_double() == 2.5);
    // An accessor for the wrong kind returns a fallback, no crash
    REQUIRE(d.as_f32() == 0.0f);
    REQUIRE(d.as_i64() == 0);
    REQUIRE_FALSE(d.as_bool());

    REQUIRE(Value{true}.to_double() == 1.0);
    REQUIRE(Value{false}.to_double() == 0.0);
    REQUIRE(Value{std::int64_t{42}}.to_double() == 42.0);
}

TEST_CASE("ports.value.kind_is_exhaustive", "[ports]") {
    // Every kind has a stable short name, and they are all distinct
    const ValueKind all[] = {ValueKind::invalid,   ValueKind::f64,  ValueKind::f32,
                             ValueKind::i64,       ValueKind::boolean, ValueKind::text,
                             ValueKind::dimension, ValueKind::field_handle};

    const auto make = [](ValueKind k) -> Value {
        switch (k) {
            case ValueKind::invalid: return Value{};
            case ValueKind::f64: return Value{1.0};
            case ValueKind::f32: return Value{1.0f};
            case ValueKind::i64: return Value{std::int64_t{1}};
            case ValueKind::boolean: return Value{true};
            case ValueKind::text: return Value{std::string{"a"}};
            case ValueKind::dimension: return Value{Dim{}};
            case ValueKind::field_handle: return Value{qp::abi::LatticeDesc{}};
        }
        return Value{};
    };

    std::vector<std::string> names;
    for (ValueKind k : all) {
        const Value v = make(k);
        REQUIRE(v.kind() == k);
        const std::string name = v.kind_name();
        REQUIRE_FALSE(name.empty());
        names.push_back(name);
    }
    // Short names are pairwise distinct: otherwise the log cannot tell which value it is
    for (std::size_t i = 0; i < names.size(); ++i) {
        for (std::size_t j = i + 1; j < names.size(); ++j) {
            INFO("i=" << i << " name=" << names[i] << "  j=" << j << " name=" << names[j]);
            REQUIRE(names[i] != names[j]);
        }
    }
}

TEST_CASE("ports.value.copy_independence", "[ports]") {
    // Value is self-contained: the copy stays usable after the original dies (diagnostics and values cross threads)
    std::string text;
    {
        const Value origin{std::string{"hello"}};
        const Value copy = origin;
        text = copy.as_text();
    }
    REQUIRE(text == "hello");

    const Value a{Dim{1, 0, -1, 0, 0, 0, 0}};
    Value b = a;
    REQUIRE(b.as_dimension() == a.as_dimension());
}

TEST_CASE("ports.value.never_throws", "[ports]") {
    // No accessor may throw: a plugin with a wrong kind check should get a diagnosable result, not an exception
    STATIC_REQUIRE(noexcept(std::declval<const Value&>().as_f64()));
    STATIC_REQUIRE(noexcept(std::declval<const Value&>().as_f32()));
    STATIC_REQUIRE(noexcept(std::declval<const Value&>().as_i64()));
    STATIC_REQUIRE(noexcept(std::declval<const Value&>().as_bool()));
    STATIC_REQUIRE(noexcept(std::declval<const Value&>().as_text()));
    STATIC_REQUIRE(noexcept(std::declval<const Value&>().as_dimension()));
    STATIC_REQUIRE(noexcept(std::declval<const Value&>().as_field()));
    STATIC_REQUIRE(noexcept(std::declval<const Value&>().to_double()));
    STATIC_REQUIRE(noexcept(std::declval<const Value&>().kind()));
    STATIC_REQUIRE(noexcept(std::declval<const Value&>().kind_name()));
}

TEST_CASE("ports.value.widening_is_lossless_for_f32", "[ports][property]") {
    // The central claim of ADR-0005: f32 -> double is lossless.
    // This is the mathematical premise that makes "one widening, at the port boundary" hold.
    const float samples[] = {0.0f,    1.0f,      -1.0f,    0.5f,   1.0f / 3.0f,
                             1e-30f,  1e30f,     -3.7e-5f, 65504.0f /* f32 max value */};
    for (float f : samples) {
        const Value v{f};
        const double widened = v.to_double();
        REQUIRE(static_cast<float>(widened) == f);   // lossless round trip
    }
}

TEST_CASE("ports.value.field_handle_carries_no_data", "[ports]") {
    // A field handle carries only the descriptor: this keeps a 19MB field out of a value
    auto lattice = qp::abi::make_lattice(qp::abi::LatticeKind::volume,
                                         qp::abi::ComponentKind::vector,
                                         qp::abi::ElementType::f32,
                                         qp::abi::FieldDim{}, 256, 128, 64);
    const Value v{lattice};
    REQUIRE(v.kind() == ValueKind::field_handle);
    // as_field returns LatticeDesc directly (not FieldBuffer)
    REQUIRE(v.as_field().count[0] == 256);
    REQUIRE(v.as_field().count[1] == 128);
    REQUIRE(v.as_field().kind == qp::abi::LatticeKind::volume);
    REQUIRE(v.as_field().component == qp::abi::ComponentKind::vector);
    // The value itself is small -- only a 32-byte lattice descriptor, no data
    REQUIRE(sizeof(Value) <= 64);

    // Equal field handles
    REQUIRE(Value{lattice} == Value{lattice});
    auto other = lattice;
    other.count[0] = 128;
    REQUIRE(Value{lattice} != Value{other});
}

TEST_CASE("ports.value.size_is_bounded", "[ports]") {
    // Values are copied heavily (cache keys, signal payloads) and must stay in the tens of bytes
    REQUIRE(sizeof(Value) <= 64);
    STATIC_REQUIRE(std::is_copy_constructible_v<Value>);
    STATIC_REQUIRE(std::is_move_constructible_v<Value>);
}

// ===========================================================================
// PortTypeDesc
// ===========================================================================

TEST_CASE("ports.type_desc.basic_fields", "[ports]") {
    PortTypeDesc d{};
    REQUIRE_FALSE(d.valid());
    REQUIRE(d.id == kInvalidType);
    REQUIRE_FALSE(d.is_field());
    REQUIRE_FALSE(d.is_numeric());

    d.id = kScalarF64;
    d.name = "scalar_f64";
    d.numeric = NumericKind::f64;
    REQUIRE(d.valid());
    REQUIRE(d.is_numeric());
    REQUIRE_FALSE(d.is_field());
}

TEST_CASE("ports.type_desc.dimension_constraint", "[ports]") {
    PortTypeDesc exact{};
    exact.id = kScalarF64;
    exact.name = "length";
    exact.numeric = NumericKind::f64;
    exact.constraint = DimensionConstraint::exact;
    exact.dimension = qp::units::dims::length;

    PortTypeDesc anydim{};
    anydim.id = kScalarF64;
    anydim.name = "any_scalar";
    anydim.numeric = NumericKind::f64;
    anydim.constraint = DimensionConstraint::any;

    REQUIRE(check_dimensions(exact, exact));
    REQUIRE(check_dimensions(exact, anydim));
    REQUIRE(check_dimensions(anydim, exact));
}

TEST_CASE("ports.type_desc.numeric_kind_consistency", "[ports]") {
    // The numeric field of a builtin scalar type must match its name
    const auto& reg = builtin_registry();
    REQUIRE(reg.find(kScalarF64)->numeric == NumericKind::f64);
    REQUIRE(reg.find(kScalarF32)->numeric == NumericKind::f32);
    REQUIRE(reg.find(kInt64)->numeric == NumericKind::i64);
    REQUIRE(reg.find(kBool)->numeric == NumericKind::boolean);
    REQUIRE(reg.find(kString)->numeric == NumericKind::none);
    REQUIRE(reg.find(kVectorField)->numeric == NumericKind::none);

    // is_field and field_components agree for field-like ports
    REQUIRE(reg.find(kScalarField)->is_field());
    REQUIRE(reg.find(kScalarField)->field_components == 1);
    REQUIRE(reg.find(kVectorField)->is_field());
    REQUIRE(reg.find(kVectorField)->field_components == 3);
    REQUIRE_FALSE(reg.find(kScalarF64)->is_field());
}

// ===========================================================================
// PortTypeRegistry
// ===========================================================================

TEST_CASE("ports.registry.builtins_present", "[ports]") {
    const auto& reg = builtin_registry();
    // Every known builtin type is present
    for (PortTypeId id : {kScalarF64, kScalarF32, kBool, kInt64, kString, kEnum, kDimension,
                          kScalarField, kVectorField, kFieldTable, kParticleBuffer, kGeometry,
                          kDataset, kFitResult, kAny}) {
        INFO("missing type id = " << id);
        REQUIRE(reg.find(id) != nullptr);
        REQUIRE(reg.has_builtin(id));
    }
    REQUIRE(reg.count() >= 15);
    REQUIRE(reg.find(kInvalidType) == nullptr);
}

TEST_CASE("ports.registry.count_and_lookup", "[ports]") {
    PortTypeRegistry reg;   // construction already contains all builtin types
    const std::size_t n = reg.count();
    REQUIRE(n > 0);

    const PortTypeDesc* d = reg.find(kDataset);
    REQUIRE(d != nullptr);
    REQUIRE(d->name == "dataset");

    REQUIRE(reg.find(kInvalidType) == nullptr);
    REQUIRE(reg.find(kUserTypeBase + 500) == nullptr);   // not registered
    REQUIRE(reg.count() == n);                            // a query does not change the registry
}

TEST_CASE("ports.registry.lookup_by_name", "[ports]") {
    const auto& reg = builtin_registry();
    REQUIRE(reg.find_by_name("scalar_f64") != nullptr);
    REQUIRE(reg.find_by_name("scalar_f64")->id == kScalarF64);
    REQUIRE(reg.find_by_name("vector_field") != nullptr);
    REQUIRE(reg.find_by_name("dataset")->id == kDataset);
    REQUIRE(reg.find_by_name("no_such_type") == nullptr);
    REQUIRE(reg.find_by_name("") == nullptr);
}

TEST_CASE("ports.registry.add_custom_type", "[ports]") {
    PortTypeRegistry reg;
    const std::size_t before = reg.count();

    PortTypeDesc custom{};
    custom.id = kUserTypeBase + 1;
    custom.name = "my_spring_state";
    custom.numeric = NumericKind::none;
    custom.constraint = DimensionConstraint::any;

    REQUIRE(reg.register_type(custom));
    REQUIRE(reg.count() == before + 1);
    REQUIRE(reg.find(kUserTypeBase + 1) != nullptr);
    REQUIRE(reg.find_by_name("my_spring_state") != nullptr);
    REQUIRE_FALSE(reg.has_builtin(kUserTypeBase + 1));
}

TEST_CASE("ports.registry.rejects_duplicate_id", "[ports]") {
    PortTypeRegistry reg;
    PortTypeDesc a{};
    a.id = kUserTypeBase + 2;
    a.name = "type_a";
    PortTypeDesc b{};
    b.id = kUserTypeBase + 2;   // same ID
    b.name = "type_b";          // different name

    REQUIRE(reg.register_type(a));
    // Silent overwrite is the most dangerous default: two plugins' type definitions would trample each other
    const auto r = reg.register_type(b);
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == qp::diag::ErrorCode::duplicate_connection);
    // Strong guarantee: the registry is unchanged after a failure
    REQUIRE(reg.find(kUserTypeBase + 2)->name == "type_a");
}

TEST_CASE("ports.registry.idempotent_reregister", "[ports]") {
    PortTypeRegistry reg;
    PortTypeDesc d{};
    d.id = kUserTypeBase + 3;
    d.name = "twice";
    d.numeric = NumericKind::f64;
    d.dimension = qp::units::dims::length;

    REQUIRE(reg.register_type(d));
    const std::size_t n = reg.count();
    // Exactly identical -> allowed (a plugin loading twice is normal)
    REQUIRE(reg.register_type(d));
    REQUIRE(reg.count() == n);
}

TEST_CASE("ports.registry.rejects_reserved_range", "[ports]") {
    PortTypeRegistry reg;
    PortTypeDesc d{};
    d.id = 500;   // < kUserTypeBase and not taken by a builtin
    d.name = "sneaky";
    const auto r = reg.register_type(d);
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == qp::diag::ErrorCode::out_of_range);
}

TEST_CASE("ports.registry.rejects_invalid_id", "[ports]") {
    PortTypeRegistry reg;
    PortTypeDesc d{};
    d.id = kInvalidType;
    d.name = "bad";
    REQUIRE_FALSE(reg.register_type(d));
    REQUIRE(reg.register_type(d).error() == qp::diag::ErrorCode::invalid_argument);

    PortTypeDesc no_name{};
    no_name.id = kUserTypeBase + 9;
    no_name.name = "";
    REQUIRE_FALSE(reg.register_type(no_name));
    REQUIRE(reg.register_type(no_name).error() == qp::diag::ErrorCode::missing_field);
}

TEST_CASE("ports.registry.deterministic_order", "[ports]") {
    // The order of all() must be stable: the UI and document generation depend on it, or the menu order changes every start
    const auto& reg = builtin_registry();
    const auto& first = reg.all();
    REQUIRE(first.size() == reg.count());
    // Builtin types appear in make_builtin_types order
    REQUIRE(first.front().id == kScalarF64);
    // Enumerate once to confirm every one can be found
    for (const auto& d : first) {
        REQUIRE(reg.find(d.id) != nullptr);
        REQUIRE(reg.find(d.id)->name == d.name);
    }
}

TEST_CASE("ports.registry.shared_builtins_is_readonly", "[ports]") {
    // The shared registry is read-only: a caller that must register a custom type needs its own
    // instance. That way "who registered what" stays local forever.
    const auto& a = builtin_registry();
    const auto& b = builtin_registry();
    REQUIRE(&a == &b);   // the same object
    const std::size_t n = a.count();
    REQUIRE(b.count() == n);
}

// ===========================================================================
// check_connection / check_value
// ===========================================================================

namespace {

PortTypeDesc make_scalar(std::string_view name, PortTypeId id, NumericKind nk, Dim dim,
                         DimensionConstraint cc = DimensionConstraint::exact) {
    PortTypeDesc d{};
    d.id = id;
    d.name = name;
    d.numeric = nk;
    d.constraint = cc;
    d.dimension = dim;
    return d;
}

}  // namespace

TEST_CASE("ports.check.connect_same_type", "[ports]") {
    const auto len = make_scalar("length", kScalarF64, NumericKind::f64, qp::units::dims::length);
    const auto r = check_connection(len, PortDirection::output, len, PortDirection::input);
    REQUIRE(r.verdict == ConnectionVerdict::ok);
    REQUIRE(r.acceptable());
    REQUIRE_FALSE(r.has_any);
    REQUIRE_FALSE(r.needs_numeric_conversion);
}

TEST_CASE("ports.check.connect_rejects_direction", "[ports]") {
    const auto len = make_scalar("length", kScalarF64, NumericKind::f64, qp::units::dims::length);
    // output -> output
    REQUIRE(check_connection(len, PortDirection::output, len, PortDirection::output).verdict ==
            ConnectionVerdict::direction_mismatch);
    // input -> input
    REQUIRE(check_connection(len, PortDirection::input, len, PortDirection::input).verdict ==
            ConnectionVerdict::direction_mismatch);
    // input -> output is legal (normalized internally to output -> input)
    REQUIRE(check_connection(len, PortDirection::input, len, PortDirection::output).verdict ==
            ConnectionVerdict::ok);
}

TEST_CASE("ports.check.connect_rejects_type_mismatch", "[ports]") {
    const auto num = make_scalar("n", kScalarF64, NumericKind::f64, Dim{});
    const auto txt = make_scalar("t", kString, NumericKind::none, Dim{},
                                 DimensionConstraint::any);
    REQUIRE(check_connection(num, PortDirection::output, txt, PortDirection::input).verdict ==
            ConnectionVerdict::type_mismatch);
    // An integer into a float must be rejected too: avoid silently losing bits
    const auto i = make_scalar("i", kInt64, NumericKind::i64, Dim{});
    REQUIRE(check_connection(i, PortDirection::output, num, PortDirection::input).verdict ==
            ConnectionVerdict::type_mismatch);
}

TEST_CASE("ports.check.connect_rejects_dimension_mismatch", "[ports]") {
    const auto len = make_scalar("len", kScalarF64, NumericKind::f64, qp::units::dims::length);
    const auto tim = make_scalar("tim", kScalarF64, NumericKind::f64, qp::units::dims::time);
    const auto r = check_connection(len, PortDirection::output, tim, PortDirection::input);
    // Same shape, different dimension -- exactly the error that load time must catch
    REQUIRE(r.verdict == ConnectionVerdict::dimension_mismatch);
    REQUIRE_FALSE(r.acceptable());
}

TEST_CASE("ports.check.connect_allows_numeric_widening", "[ports]") {
    const auto f32dim = make_scalar("f32", kScalarF32, NumericKind::f32, Dim{});
    const auto f64dim = make_scalar("f64", kScalarF64, NumericKind::f64, Dim{});

    // f32 -> f64: lossless widening
    const auto widen = check_connection(f32dim, PortDirection::output, f64dim,
                                        PortDirection::input);
    REQUIRE(widen.verdict == ConnectionVerdict::ok_with_numeric_widening);
    REQUIRE(widen.acceptable());
    REQUIRE(widen.needs_numeric_conversion);

    // f64 -> f32: lossy narrowing, but allowed (an explicit choice by the caller)
    const auto narrow = check_connection(f64dim, PortDirection::output, f32dim,
                                         PortDirection::input);
    REQUIRE(narrow.acceptable());
    REQUIRE(narrow.needs_numeric_conversion);
}

TEST_CASE("ports.check.connect_any_is_flagged", "[ports]") {
    // any is an escape hatch: the connection is allowed but must leave an explicit trace, or the type-safety hole would exist silently
    const auto& reg = builtin_registry();
    const auto* any_type = reg.find(kAny);
    const auto len = make_scalar("len", kScalarF64, NumericKind::f64, qp::units::dims::length);

    const auto r = check_connection(len, PortDirection::output, *any_type, PortDirection::input);
    REQUIRE(r.verdict == ConnectionVerdict::ok_with_any);
    REQUIRE(r.acceptable());
    REQUIRE(r.has_any);
}

TEST_CASE("ports.check.connect_unknown_type", "[ports]") {
    PortTypeDesc bad{};   // id == kInvalidType
    const auto len = make_scalar("len", kScalarF64, NumericKind::f64, qp::units::dims::length);
    REQUIRE(check_connection(bad, PortDirection::output, len, PortDirection::input).verdict ==
            ConnectionVerdict::unknown_type);
    REQUIRE(check_connection(len, PortDirection::output, bad, PortDirection::input).verdict ==
            ConnectionVerdict::unknown_type);
}

TEST_CASE("ports.check.connect_is_symmetric_for_mismatch", "[ports][property]") {
    // Shape/dimension mismatches do not depend on which side is left and which is right
    const auto len = make_scalar("len", kScalarF64, NumericKind::f64, qp::units::dims::length);
    const auto tim = make_scalar("tim", kScalarF64, NumericKind::f64, qp::units::dims::time);
    const auto txt = make_scalar("t", kString, NumericKind::none, Dim{},
                                 DimensionConstraint::any);

    REQUIRE(check_connection(len, PortDirection::output, tim, PortDirection::input).verdict ==
            check_connection(tim, PortDirection::output, len, PortDirection::input).verdict);
    REQUIRE(check_connection(len, PortDirection::output, txt, PortDirection::input).verdict ==
            check_connection(txt, PortDirection::output, len, PortDirection::input).verdict);
}

TEST_CASE("ports.check.dimension_compatible", "[ports]") {
    const auto len1 = make_scalar("l1", kScalarF64, NumericKind::f64, qp::units::dims::length);
    const auto len2 = make_scalar("l2", kScalarF64, NumericKind::f64, qp::units::dims::length);
    const auto tim = make_scalar("t", kScalarF64, NumericKind::f64, qp::units::dims::time);
    REQUIRE(check_dimensions(len1, len2));
    REQUIRE_FALSE(check_dimensions(len1, tim));
    // Aliases with the same dimension but different meaning (Energy / Torque) are compatible at the
    // dimension level -- a known limit of the dimension system (core/units/spec.md U2), not faked here.
    REQUIRE(check_dimensions(len1, len1));
}

TEST_CASE("ports.check.dimension_any_accepts_all", "[ports]") {
    const auto len = make_scalar("l", kScalarF64, NumericKind::f64, qp::units::dims::length);
    const auto anydim = make_scalar("a", kScalarF64, NumericKind::f64, Dim{},
                                    DimensionConstraint::any);
    REQUIRE(check_dimensions(len, anydim));
    REQUIRE(check_dimensions(anydim, len));
    REQUIRE(check_dimensions(anydim, anydim));
}

TEST_CASE("ports.check.dimension_same_as_input_is_deferred", "[ports]") {
    // same_as_input needs graph context; the port layer does not decide it, leaving it to core/graph/validate
    const auto len = make_scalar("l", kScalarF64, NumericKind::f64, qp::units::dims::length);
    const auto derived = make_scalar("d", kScalarF64, NumericKind::f64, Dim{},
                                     DimensionConstraint::same_as_input);
    REQUIRE(check_dimensions(len, derived));
    REQUIRE(check_dimensions(derived, len));
}

TEST_CASE("ports.check.value_matches_port", "[ports]") {
    const auto f64len = make_scalar("len", kScalarF64, NumericKind::f64, qp::units::dims::length);
    REQUIRE(check_value(f64len, Value{2.5}));

    const auto b = [&] {
        PortTypeDesc d{};
        d.id = kBool;
        d.name = "bool";
        d.numeric = NumericKind::boolean;
        d.constraint = DimensionConstraint::any;
        return d;
    }();
    REQUIRE(check_value(b, Value{true}));

    const auto s = [&] {
        PortTypeDesc d{};
        d.id = kString;
        d.name = "string";
        d.numeric = NumericKind::none;
        d.constraint = DimensionConstraint::any;
        return d;
    }();
    REQUIRE(check_value(s, Value{std::string{"ok"}}));
}

TEST_CASE("ports.check.value_rejects_wrong_kind", "[ports]") {
    const auto f64len = make_scalar("len", kScalarF64, NumericKind::f64, qp::units::dims::length);
    // Text into a numeric port
    REQUIRE_FALSE(check_value(f64len, Value{std::string{"nope"}}));
    // Integer into a float port
    REQUIRE_FALSE(check_value(f64len, Value{std::int64_t{3}}));

    const auto i = [&] {
        PortTypeDesc d{};
        d.id = kInt64;
        d.name = "int64";
        d.numeric = NumericKind::i64;
        d.constraint = DimensionConstraint::any;
        return d;
    }();
    // Float into an integer port
    REQUIRE_FALSE(check_value(i, Value{2.5}));
    REQUIRE(check_value(i, Value{std::int64_t{3}}));
}

TEST_CASE("ports.check.value_accepts_widening", "[ports]") {
    const auto f64len = make_scalar("len", kScalarF64, NumericKind::f64, qp::units::dims::length);
    const auto f32len = make_scalar("len32", kScalarF32, NumericKind::f32,
                                    qp::units::dims::length);
    // An f32 value may enter an f64 port (lossless)
    REQUIRE(check_value(f64len, Value{1.5f}));
    // An f64 value may enter an f32 port (lossy but explicitly allowed)
    REQUIRE(check_value(f32len, Value{1.5}));
}

TEST_CASE("ports.check.value_any_accepts_all", "[ports]") {
    const auto* any_type = builtin_registry().find(kAny);
    REQUIRE(check_value(*any_type, Value{1.0}));
    REQUIRE(check_value(*any_type, Value{std::string{"x"}}));
    REQUIRE(check_value(*any_type, Value{}));   // an unevaluated value is legal for any too
}

TEST_CASE("ports.check.value_rejects_invalid", "[ports]") {
    const auto f64len = make_scalar("len", kScalarF64, NumericKind::f64, qp::units::dims::length);
    REQUIRE_FALSE(check_value(f64len, Value{}));

    PortTypeDesc bad{};
    REQUIRE_FALSE(check_value(bad, Value{1.0}));
    REQUIRE(check_value(bad, Value{1.0}).error() == qp::diag::ErrorCode::unknown_port_type);
}

TEST_CASE("ports.check.value_field_components_matter", "[ports]") {
    const auto* scalar_field = builtin_registry().find(kScalarField);
    const auto* vector_field = builtin_registry().find(kVectorField);
    REQUIRE(scalar_field != nullptr);
    REQUIRE(vector_field != nullptr);

    const auto scalar_lattice = qp::abi::make_lattice(
        qp::abi::LatticeKind::line, qp::abi::ComponentKind::scalar, qp::abi::ElementType::f32,
        qp::abi::FieldDim{}, 10);
    const auto vector_lattice = qp::abi::make_lattice(
        qp::abi::LatticeKind::line, qp::abi::ComponentKind::vector, qp::abi::ElementType::f32,
        qp::abi::FieldDim{}, 10);

    REQUIRE(check_value(*scalar_field, Value{scalar_lattice}));
    REQUIRE_FALSE(check_value(*scalar_field, Value{vector_lattice}));
    REQUIRE(check_value(*vector_field, Value{vector_lattice}));
    REQUIRE_FALSE(check_value(*vector_field, Value{scalar_lattice}));
    // A non-field value into a field port
    REQUIRE_FALSE(check_value(*vector_field, Value{1.0}));
}
