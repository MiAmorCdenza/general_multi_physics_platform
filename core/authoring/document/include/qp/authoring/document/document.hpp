/**
 * @file document.hpp
 * @brief Document identity and the layout metadata that belongs to a view, not to the graph.
 *
 * ## The one idea in this file
 *
 * A node's position on screen is **not a property of the graph**. The graph says
 * "a spring is connected to a mass"; it does not say "and the spring is drawn at
 * (120, 80)". Anything that puts coordinates into the graph makes every other
 * view wrong: a time-series plot has no x, an equation view has no x, and a
 * script that reads the document would have to skip a field it cannot interpret.
 *
 * So coordinates live in `ViewLayouts`, keyed by **view id**, and the core never
 * reads them. The core does not know what "x" means, and must not: the moment it
 * does, adding a view becomes a core change, which is the thing the whole
 * architecture is arranged to avoid.
 *
 * The consequence worth stating plainly: **a user who has not installed the node
 * editor simply has no `graph` slot in their document.** Nothing is missing and
 * nothing is malformed -- there is no layout for a view that does not exist.
 *
 * ## Why the payload is opaque bytes
 *
 * A layout is whatever the view needs: a number pair, a JSON object, a binary
 * blob from a plugin's own layout algorithm. The core stores it, hands it back,
 * and refuses to interpret it. Typing it as a structured value would mean the
 * core owning a schema for every view that will ever exist.
 *
 * ## Why this is not the session
 *
 * `authoring/commands::Session` owns the **live** graph being edited. This module
 * owns what a **saved** document is: identity plus the per-view metadata that
 * travels with it. Keeping them apart is what stops "the document" from becoming
 * a second mutable handle on the graph, which would reintroduce exactly the
 * two-copies problem the session exists to prevent.
 *
 * @ownership   owns (identity, and each view's bytes)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A view id appears at most once, and only while it holds a layout
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       authoring.document.layouts_are_slotted_by_view,
 *              authoring.document.absent_view_has_no_slot,
 *              authoring.document.layout_roundtrip
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qp::authoring {

/// @brief A view's identity, e.g. "graph", "timeseries", "script".
using ViewId = std::string;

/**
 * @brief Per-view layout metadata, one slot per view that has something to store.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Views are stored in first-set order, so saving twice writes the same bytes
 * @errors      noexcept
 * @frozen      no
 * @tests       authoring.document.layouts_are_slotted_by_view
 */
class ViewLayouts final {
public:
    ViewLayouts() = default;

    /**
     * @brief Stores a view's layout, replacing any previous value for that view.
     *
     * Replacing rather than appending: a view that saved twice would otherwise
     * accumulate two layouts for one id, and whichever a reader picked would be
     * an accident.
     *
     * @ownership   owns (copies `data`)
     * @thread      main
     * @pre         `view` is non-empty
     * @post        has(view) is true and get(view) returns `data`
     * @invariant   The view keeps its original position in the order
     * @errors      noexcept
     * @complexity  O(views) plus the copy
     * @nondet      none
     * @frozen      no
     * @tests       authoring.document.layout_roundtrip
     */
    void set(const ViewId& view, std::string data) noexcept;

    /// @brief The layout stored for `view`, or an empty string when absent.
    [[nodiscard]] const std::string& get(const ViewId& view) const noexcept;

    /// @brief Whether `view` has a slot. False means "that view is not installed".
    [[nodiscard]] bool has(const ViewId& view) const noexcept;

    /**
     * @brief Removes a view's slot. Returns whether there was one.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        has(view) is false
     * @invariant   Other views keep their order
     * @errors      noexcept
     * @complexity  O(views)
     * @nondet      none
     * @frozen      no
     * @tests       authoring.document.removing_a_view_leaves_the_rest
     */
    bool remove(const ViewId& view) noexcept;

    /// @brief View ids in first-set order.
    [[nodiscard]] std::vector<ViewId> view_ids() const noexcept;

    /// @brief Number of views holding a layout.
    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

    /// @brief Whether no view holds a layout.
    [[nodiscard]] bool empty() const noexcept { return slots_.empty(); }

    /// @brief Drops every slot.
    void clear() noexcept { slots_.clear(); }

private:
    struct Slot final {
        ViewId view{};
        std::string data{};
    };

    [[nodiscard]] const Slot* find(const ViewId& view) const noexcept;

    std::vector<Slot> slots_;
};

/**
 * @brief What a saved document is: a name, a place, and the views' metadata.
 *
 * The graph itself is not stored here. A document **describes** a graph; it does
 * not hold a second one. A serializer takes the graph from the session and the
 * layouts from here, and writes both.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `source_path` is empty for a document that has never been saved
 * @errors      noexcept
 * @frozen      no
 * @tests       authoring.document.identity_and_dirty_state
 */
class Document final {
public:
    Document() = default;

    /// @brief A human-readable title, shown in a window caption or a tab.
    [[nodiscard]] const std::string& title() const noexcept { return title_; }

    /**
     * @brief Sets the title.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        title() returns the new value
     * @invariant   Does not change the source path
     * @errors      noexcept
     * @complexity  O(len)
     * @nondet      none
     * @frozen      no
     * @tests       authoring.document.identity_and_dirty_state
     */
    void set_title(std::string title) noexcept { title_ = std::move(title); }

    /// @brief Where this document was last saved, or empty if it never was.
    [[nodiscard]] const std::string& source_path() const noexcept { return source_path_; }

    /**
     * @brief Records where the document was saved.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        source_path() returns the new value
     * @invariant   Saving does not change the title: a user who renamed a window
     *              should not have the name revert because they saved elsewhere
     * @errors      noexcept
     * @complexity  O(len)
     * @nondet      none
     * @frozen      no
     * @tests       authoring.document.save_as_keeps_the_title
     */
    void set_source_path(std::string path) noexcept { source_path_ = std::move(path); }

    /// @brief Whether the document has never been written to disk.
    [[nodiscard]] bool is_untitled() const noexcept { return source_path_.empty(); }

    /// @brief The per-view layout metadata travelling with this document.
    [[nodiscard]] ViewLayouts& layouts() noexcept { return layouts_; }
    [[nodiscard]] const ViewLayouts& layouts() const noexcept { return layouts_; }

    /**
     * @brief Whether edits have happened since the document was last saved.
     *
     * Set by the host rather than derived here: this module does not own the
     * graph, so it cannot tell whether the graph changed. A document that guessed
     * would either warn about unsaved changes that do not exist, or fail to warn
     * about ones that do -- and the second is how work gets lost.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   A freshly constructed document is clean
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       authoring.document.identity_and_dirty_state
     */
    [[nodiscard]] bool is_dirty() const noexcept { return dirty_; }
    void mark_dirty() noexcept { dirty_ = true; }
    void mark_saved() noexcept { dirty_ = false; }

private:
    std::string title_{};
    std::string source_path_{};
    ViewLayouts layouts_{};
    bool dirty_ = false;
};

}  // namespace qp::authoring
