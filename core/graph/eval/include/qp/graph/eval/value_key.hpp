/**
 * @file value_key.hpp
 * @brief Content addressing: reproducible value hashes and key construction.
 *
 * ## Why not `std::hash`
 *
 * `std::hash<std::string>` and `std::hash<double>` are **implementation-defined**:
 * one binary gives different results on different standard library versions, so:
 *
 *   1. **Not reproducible across runs**. A cache written to disk fails to match on read-back.
 *   2. **Hit rate cannot be explained**. "Why did this miss?" becomes unanswerable.
 *
 * So this project ships its own deterministic hash (FNV-1a 64) and writes it into the contract.
 * It is not a cryptographic hash -- it is only for cache keys, and it **must** be used with
 * an exact comparison (see `CacheKey::equals`): equal hashes do not mean equal keys.
 *
 * ## Large objects are keyed by identity, never by content
 *
 * A 19MB field cannot be hashed byte by byte. So a value of type `field_handle` takes
 * part in key construction through its **lattice descriptor** -- the anchor point of the
 * "same lattice = same data" convention, guaranteed by the publisher of `core/abi`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   One sequence of values always yields the same hash (across runs and compilers)
 * @errors      noexcept
 * @frozen      yes (algorithm frozen; changing it invalidates every cache)
 */
#pragma once

#include <qp/graph/ir/descriptor.hpp>
#include <qp/graph/ir/ids.hpp>
#include <qp/ports/value.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace qp::graph {

/// @brief Type alias for a hash value.
using ValueHash = std::uint64_t;

/// @brief FNV-1a 64 offset basis (named as in the spec, to ease cross-checking).
inline constexpr ValueHash kFnvOffsetBasis = 0xcbf29ce484222325ULL;
/// @brief FNV-1a 64 prime.
inline constexpr ValueHash kFnvPrime = 0x100000001b3ULL;

/**
 * @brief Mixes a byte string into a hash. FNV-1a.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns the new mixed hash; the input is not modified
 * @invariant   One (seed, bytes) pair always yields the same result
 * @errors      noexcept
 * @complexity  O(len)
 * @nondet      none
 * @frozen      yes
 * @tests       graph.eval.hash_is_fnv1a, graph.eval.hash_of_empty_is_seed,
 *              graph.eval.hash_is_deterministic
 */
[[nodiscard]] ValueHash mix_bytes(ValueHash seed, const void* data, std::size_t len) noexcept;

/// @brief Mixes in a 64-bit integer (little-endian bytes, for cross-platform consistency).
[[nodiscard]] ValueHash mix_u64(ValueHash seed, std::uint64_t v) noexcept;

/**
 * @brief Computes the deterministic hash of one value.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        One value always yields the same hash; different kinds of value never
 *              collide (the kind tag is mixed in first)
 * @invariant   Deterministic: depends on no address, time or random number
 * @errors      noexcept
 * @complexity  O(1) (except for text)
 * @nondet      none
 * @frozen      yes
 * @tests       graph.eval.hash_value_kind_discriminates,
 *              graph.eval.hash_value_f32_f64_differ,
 *              graph.eval.hash_field_uses_lattice
 */
[[nodiscard]] ValueHash hash_value(ValueHash seed, const qp::ports::Value& v) noexcept;

/**
 * @brief Computes the deterministic hash of a sequence of values (paired with port numbers).
 *
 * The port number is part of the hash: `{1: 2.0}` and `{2: 2.0}` must differ,
 * otherwise "move the wire from port 1 to port 2" would hit the same cache entry.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Order-sensitive (mixed in the given order)
 * @invariant   Deterministic
 * @errors      noexcept
 * @complexity  O(n)
 * @nondet      none
 * @frozen      yes
 * @tests       graph.eval.hash_port_number_matters, graph.eval.hash_order_matters
 */
[[nodiscard]] ValueHash hash_port_values(ValueHash seed,
                                         const std::vector<std::pair<PortNumber,
                                                                     qp::ports::Value>>& values)
    noexcept;

/**
 * @brief Serializes a value to readable text. For **exact cache-key comparison** and debugging.
 *
 * The hash does the fast filtering, the text makes the final call: no false hit on a collision.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns a non-empty string; one value always yields the same text
 * @invariant   If two values have different text, they are not the same value
 * @errors      noexcept (allocation failure terminates)
 * @complexity  O(1) (except for text)
 * @nondet      none
 * @frozen      no (display format may change; not part of cache-key equality)
 * @tests       graph.eval.canonical_text_is_stable, graph.eval.canonical_text_discriminates
 */
[[nodiscard]] std::string canonical_text(const qp::ports::Value& v) noexcept;

}  // namespace qp::graph
