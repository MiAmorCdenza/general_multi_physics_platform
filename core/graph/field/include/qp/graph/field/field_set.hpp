/**
 * @file field_set.hpp
 * @brief Where a baked field's samples live, and how a port value finds them.
 *
 * ## The gap this closes
 *
 * `ports::Value`'s `field_handle` alternative carries a **`abi::LatticeDesc` and nothing else** -- by design,
 * because a 19 MB field must never travel inside a value, and the header says so: "the real data lifetime is
 * guaranteed by the publisher in `core/abi`". Until this file there **was no publisher**. `field_handle`
 * round-tripped through the JSON format, the port-type registry knew a vector field from a scalar one, and
 * nothing anywhere could turn a handle into samples. That is the same shape this repository keeps finding: a
 * contract that is declared, documented, unit-tested, and unreachable from the running program.
 *
 * So: a `FieldSet` owns published sample buffers, keyed by **who published them**. A bake writes here; the
 * descriptor in the port value names the entry; a consumer that holds the descriptor and the set can read the
 * data.
 *
 * ## Why the key is a node and a port, and not the descriptor
 *
 * Keying by lattice description looks tempting -- the description is already in the handle -- and it is wrong:
 * two nodes that happen to bake the same grid shape would share an entry, so the second bake would overwrite the
 * first and a graph with two dipoles at different tilts would run one of them twice. The identity of a baked
 * field is **which node produced it**, which is exactly what the plan knows and what the handle's publisher can
 * key on. The port is part of the key because a node may publish more than one field.
 *
 * The key is two integers and not a `graph::NodeId`, deliberately: this module depends on `abi` and knows
 * nothing about nodes and edges, and a store that had to know what a NodeId is could not be used by a baker that
 * has no graph. The mapping from a node to its key is one line in whoever calls `publish`.
 *
 * ## f64 only, and the reopening condition
 *
 * Samples are stored as `f64`. The particle path is double precision by the platform owner's decision
 * (`particle_state.hpp` argues it: a Boris push accumulates hundreds of thousands of steps and the conservation
 * checks are differences of numbers of order one, so in f32 "energy is conserved" and "energy drifts by the
 * rounding" are indistinguishable). An `f32` descriptor is therefore **refused with a false** rather than
 * narrowed silently: a bake that produced f32 samples and got a handle back would be a run whose precision was
 * decided by a fallback nobody wrote down. The reopening condition is a *render-only* field large enough that
 * its bytes matter -- a 19 MB f32 field is 38 MB here -- and the change is a second storage vector plus a
 * descriptor-driven choice in `view`, not a different key or a different lifetime rule.
 *
 * ## What a `FieldValue` taken from here may outlive
 *
 * A published entry's samples live on the heap and are never moved: inserting or erasing *other* entries moves
 * `Entry` objects between slots, and moving a `std::vector` does not move the bytes it points at. So a
 * `FieldValue` from `view` stays valid until its own key is published again or erased, or the set is destroyed
 * or cleared. That is a weaker promise than "forever" and it is the honest one, because a re-bake must be able
 * to replace the samples the run is reading.
 *
 * @ownership   owns the sample buffers it publishes
 * @thread      main (publish, erase, clear) / eval (view)
 * @pre         none
 * @post        none
 * @invariant   Every stored entry's bytes equal `abi::data_bytes` of its descriptor
 * @errors      See each declaration
 * @frozen      no
 * @tests       field.set.publishes_and_reads_back, field.set.refuses_what_it_cannot_own,
 *              field.set.a_rebake_replaces_the_samples, field.set.keys_are_ordered_and_distinct
 */
#pragma once

#include <qp/abi/lattice.hpp>
#include <qp/graph/field/field.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qp::graph::field {

/**
 * @brief Who published a field: the node that produced it, and the port it came out of.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `node == 0` is the "no field" sentinel and can never be published
 * @errors      noexcept
 * @frozen      no
 * @tests       field.set.keys_are_ordered_and_distinct
 */
struct FieldKey final {
    /// The publishing node's slot index. Zero means "none": a node index of zero is the graph's own invalid slot.
    std::uint32_t node = 0;
    /// The output port the field was published on.
    std::uint32_t port = 0;

    [[nodiscard]] friend constexpr bool operator==(FieldKey a, FieldKey b) noexcept {
        return a.node == b.node && a.port == b.port;
    }
    [[nodiscard]] friend constexpr bool operator!=(FieldKey a, FieldKey b) noexcept {
        return !(a == b);
    }
    /// @brief Ordering by node, then port. What makes a `FieldSet`'s contents enumerable in a fixed order.
    [[nodiscard]] friend constexpr bool operator<(FieldKey a, FieldKey b) noexcept {
        return a.node != b.node ? a.node < b.node : a.port < b.port;
    }
    /// @brief Whether this key names a field at all.
    [[nodiscard]] constexpr bool valid() const noexcept { return node != 0; }
};

/// @brief The sentinel "no field". `view` of it returns an unreadable value rather than failing.
inline constexpr FieldKey kNoField{};

/**
 * @brief The published fields of one run.
 *
 * @ownership   owns
 * @thread      main (publish, erase, clear) / eval (view)
 * @pre         none
 * @post        none
 * @invariant   Entries are ordered by key and no two share one
 * @errors      See each declaration
 * @frozen      no
 * @tests       field.set.publishes_and_reads_back
 */
class FieldSet final {
public:
    /// @brief An empty set.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        `size()` is zero
    /// @invariant   No allocation
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       field.set.publishes_and_reads_back
    FieldSet() = default;

    /**
     * @brief Publishes one field, taking ownership of its samples.
     *
     * Takes the vector **by value** rather than by reference or pointer, and that is the whole API decision: a
     * caller that already has a `std::vector<double>` moves it in and pays nothing, and a caller holding a raw
     * buffer constructs one and pays a copy it wrote down. There is no third path, so there is no path where a
     * store keeps a pointer to memory somebody else will free -- which is the failure `abi::FieldBuffer`'s
     * "the publisher keeps it alive" sentence delegates to whoever owns the publisher.
     *
     * Republishing a key **replaces** its entry, because a re-bake is the ordinary case: a parameter changed, the
     * field domain recomputed, and the run that follows must read the new samples rather than the first ones.
     *
     * @param key     Who published it. A key with `node == 0` is refused.
     * @param desc    The lattice the samples describe. Must be self-consistent, `f64`, and match `samples`.
     * @param samples The samples, in the layout `abi::LatticeDesc` describes.
     *
     * @ownership   owns
     * @thread      main
     * @pre         `desc` describes exactly as many values as `samples` holds
     * @post        On true, `view(key)` is readable and equal to `desc` and the samples
     * @invariant   On false the set is unchanged
     * @errors      Returns false -- never throws -- for a key with no node, an inconsistent descriptor, an
     *              element type other than `f64`, or a sample count that is not the descriptor's own
     * @complexity  O(points) for the move, O(entries) for the insertion
     * @nondet      none
     * @frozen      no
     * @tests       field.set.publishes_and_reads_back, field.set.refuses_what_it_cannot_own,
     *              field.set.a_rebake_replaces_the_samples
     */
    [[nodiscard]] bool publish(FieldKey key, const qp::abi::LatticeDesc& desc,
                               std::vector<double> samples);

    /**
     * @brief The field published under `key`, or an unreadable value.
     *
     * An **unreadable** `FieldValue` rather than an error code, and the choice is the same one `StepPlan::field`
     * makes: an absent field and a field of zeros are the same force and different experiments, and
     * `field::is_readable` is the predicate that tells them apart. A caller that conflated them would run a
     * particle in an absent magnetic field and see a straight line.
     *
     * @param key Which field.
     *
     * @ownership   borrows from this object until the key is republished or erased
     * @thread      eval
     * @pre         none
     * @post        A readable view for a published key, a default-constructed one otherwise
     * @invariant   The returned descriptor equals the one `publish` was given
     * @errors      noexcept
     * @complexity  O(log entries)
     * @nondet      none
     * @frozen      no
     * @tests       field.set.publishes_and_reads_back
     */
    [[nodiscard]] FieldValue view(FieldKey key) const noexcept;

    /// @brief Whether `key` names a published field.
    ///
    /// @param key Which field.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        Equivalent to `is_readable(view(key))`
    /// @invariant   Never fails
    /// @errors      noexcept
    /// @complexity  O(log entries)
    /// @nondet      none
    /// @frozen      no
    /// @tests       field.set.keys_are_ordered_and_distinct
    [[nodiscard]] bool contains(FieldKey key) const noexcept;

    /// @brief Drops one field, releasing its samples.
    ///
    /// @param key Which field.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        `contains(key)` is false
    /// @invariant   Other entries are untouched, and their sample buffers are not relocated
    /// @errors      noexcept
    /// @complexity  O(entries)
    /// @nondet      none
    /// @frozen      no
    /// @tests       field.set.keys_are_ordered_and_distinct
    bool erase(FieldKey key) noexcept;

    /// @brief Drops every field. Any `FieldValue` taken from here becomes invalid.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        `size()` is zero
    /// @invariant   The capacity of the entries vector is kept, because a run that rebakes does not want to
    ///              allocate its way back to where it was
    /// @errors      noexcept
    /// @complexity  O(entries)
    /// @nondet      none
    /// @frozen      no
    /// @tests       field.set.keys_are_ordered_and_distinct
    void clear() noexcept;

    /// @brief How many fields are published.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        At most the number of distinct keys ever published without an erase
    /// @invariant   Equals the number of keys `view` answers readably for
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       field.set.publishes_and_reads_back
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// @brief The keys, in ascending order.
    ///
    /// Published so a caller can report **what a run baked** without knowing which nodes it baked: the set is
    /// the record, and a report that could not enumerate it would have to ask the graph, which is the layer this
    /// one deliberately does not know about.
    ///
    /// @ownership   borrows from this object
    /// @thread      main
    /// @pre         none
    /// @post        One key per published field, ascending
    /// @invariant   Sorted and distinct
    /// @errors      noexcept
    /// @complexity  O(entries)
    /// @nondet      none
    /// @frozen      no
    /// @tests       field.set.keys_are_ordered_and_distinct
    [[nodiscard]] std::vector<FieldKey> keys() const;

    /// @brief Total sample bytes held, for a report that has to say what a bake cost.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        The sum of every entry's byte count
    /// @invariant   Equals the bytes `view` describes, summed
    /// @errors      noexcept
    /// @complexity  O(entries)
    /// @nondet      none
    /// @frozen      no
    /// @tests       field.set.publishes_and_reads_back
    [[nodiscard]] std::uint64_t bytes() const noexcept;

private:
    /// @brief One published field: its identity, its shape and the samples it owns.
    struct Entry final {
        FieldKey key{};
        qp::abi::LatticeDesc desc{};
        std::vector<double> samples{};
    };

    /// @brief The entry for `key`, or null. Binary search over the ordered entries.
    [[nodiscard]] const Entry* find(FieldKey key) const noexcept;

    std::vector<Entry> entries_{};
};

}  // namespace qp::graph::field
