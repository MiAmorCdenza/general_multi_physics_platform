/**
 * @file domain.hpp
 * @brief Three execution domains: their semantics are **genuinely different**, not labels.
 *
 * ## The three domains
 *
 * | Domain     | When it evaluates          | Main restrictions                                |
 * |------------|----------------------------|--------------------------------------------------|
 * | `field`    | on parameter change (bake) | may block, allocate, call Python / DLL           |
 * | `particle` | **every frame**            | no blocking, no allocation, no throw, no virtual |
 * | `render`   | **not evaluated**          | pure declaration; read by the view layer         |
 *
 * ## Why "domain" must be a foundation concept
 *
 * A realtime domain runs every frame, so its implementation constraints (no allocation,
 * no blocking, no throwing) differ completely from a bake domain. Without domains, those
 * constraints survive only by code review, and "a plugin allocated a vector inside the
 * realtime domain" is **invisible at run time** -- it just shows up as an occasional hitch.
 *
 * So a domain is **declarative**: a node type declares in its own description which
 * domains it may appear in (`NodeDesc::allow_in_field_domain` /
 * `allow_in_particle_domain`), and the validation layer enforces it at load time.
 *
 * ## Why the render domain is **not evaluated**
 *
 * A render node describes "what to draw", not "what to compute". Giving it a
 * `compute()` drags the view layer's representation into the kernel (the old project's
 * `Graph.bake()` welded one transfer format into the kernel that way). A render node
 * therefore has **no implementation**, only a declaration; view plugins do the drawing.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A node belongs to at most one domain
 * @errors      noexcept
 * @frozen      yes (enum values frozen)
 */
#pragma once

#include <cstdint>

namespace qp::graph {

/// @brief Execution domain. The numeric value is the stable tag and must not be reordered.
enum class Domain : std::uint8_t {
    /// Bake (field): offline, may block, may allocate.
    field = 0,
    /// Realtime (particle): runs every frame; no blocking, no allocation.
    particle = 1,
    /// Render (render): pure declaration, **not evaluated**.
    render = 2,
};

/// @brief Stable short name of a domain.
[[nodiscard]] constexpr const char* to_string(Domain d) noexcept {
    switch (d) {
        case Domain::field: return "field";
        case Domain::particle: return "particle";
        case Domain::render: return "render";
    }
    return "unknown";
}

/**
 * @brief Whether this domain runs every frame.
 *
 * Domains that run every frame impose hard constraints: no allocation, no block, no throw.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        particle returns true, everything else returns false
 * @invariant   Consistent with to_string
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes
 * @tests       graph.domain.predicates
 */
[[nodiscard]] constexpr bool runs_every_frame(Domain d) noexcept {
    return d == Domain::particle;
}

/**
 * @brief Whether this domain takes part in evaluation.
 *
 * The `render` domain does not: a render node has only a declaration, no `compute()`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        render returns false, everything else returns true
 * @invariant   Consistent with the "does the node have a compute implementation" rule
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes
 * @tests       graph.domain.predicates
 */
[[nodiscard]] constexpr bool participates_in_evaluation(Domain d) noexcept {
    return d != Domain::render;
}

/**
 * @brief Capability constraints of a domain. Used for **load-time** validation and docs.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every capability flag is false in the particle domain
 * @errors      noexcept
 * @frozen      no
 * @tests       graph.domain.capabilities
 */
struct DomainCapabilities final {
    bool may_block = true;         ///< May block (e.g. waiting on a solver)
    bool may_allocate = true;      ///< May allocate dynamically
    bool may_throw = true;         ///< May throw exceptions
    bool may_call_foreign = true;  ///< May call Python / DLL / MATLAB
    bool may_use_virtual = true;   ///< May use virtual calls (interface dispatch)
};

/**
 * @brief Fetches the capability constraints of a domain.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The particle domain returns a capability set with every flag false
 * @invariant   The same domain always returns the same result
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes
 * @tests       graph.domain.capabilities
 */
[[nodiscard]] constexpr DomainCapabilities capabilities_of(Domain d) noexcept {
    switch (d) {
        case Domain::field:
            return DomainCapabilities{true, true, true, true, true};
        case Domain::particle:
            // Runs every frame: all banned. Their common result is **unpredictable frame time**.
            return DomainCapabilities{false, false, false, false, false};
        case Domain::render:
            // A render declaration computes nothing; the loosest values say "not applicable".
            return DomainCapabilities{true, true, true, true, true};
    }
    return DomainCapabilities{};
}

}  // namespace qp::graph
