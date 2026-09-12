/**
 * @file execution_binders.hpp
 * @brief Where the window gets its node-to-operator binders from.
 *
 * ## The dependency problem this solves
 *
 * A binder lives in the plugin that ships the operator -- that is the whole point of the split, and
 * `views::model::RunController` takes a list of them. The obvious wiring is for the window to include
 * `plugins/mechanics/mechanics_binder.hpp` and build one.
 *
 * That does not compile, and the reason is worth keeping: `views` would then depend on `plugins` while
 * `plugins` already depends on `graph/execution`, and the build order becomes cyclic in the directory
 * graph even though the include graph looks fine. More importantly it inverts the layering --
 * `docs/plan-tree.md` says plugins are **content** that a consumer mounts, not a library the view layer
 * links against. A window that names a plugin by type cannot be built without that plugin.
 *
 * ## The inversion
 *
 * The view layer declares **where binders come from**; whoever assembles the application puts them
 * there. `views/qt` calls `execution_binders()` and mounts whatever it gets, and the application -- the
 * one place allowed to know which plugins exist -- fills the list.
 *
 * An empty list is a legitimate state and not an error: a build with `QP_BUILD_PLUGINS=OFF` has no
 * binders, and the Run action then reports "no node in this graph has an operator yet", which is true
 * and is a thing the user can act on.
 *
 * @ownership   mixed -- see each declaration
 * @thread      main (set once during startup, read on every run)
 * @pre         none
 * @post        none
 * @invariant   `execution_binders()` returns a reference that stays valid until the process ends
 * @errors      noexcept
 * @frozen      no
 * @tests       views.binders.mounted_once_and_in_order
 */
#pragma once

#include <qp/graph/execution/execution.hpp>

#include <vector>

namespace qp::views::model {

/**
 * @brief The binders the application mounted, in the order they should be consulted.
 *
 * Returns a reference to a shared list rather than a copy: a binder is owned by whoever created it and
 * the list only holds pointers, so copying it would be copying borrows and the copy would outlive
 * nothing in particular. The list is expected to be filled once, before the window runs.
 *
 * @ownership   borrows (the returned reference outlives any caller)
 * @thread      main
 * @pre         none
 * @post        Returns an empty list when nothing has been mounted
 * @invariant   The same object every call
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       views.binders.mounted_once_and_in_order
 */
[[nodiscard]] std::vector<qp::graph::execution::IOperatorBinder*>& execution_binders() noexcept;

/**
 * @brief Adds `binder` to the list the window consults.
 *
 * Called by the application during startup, once per plugin that ships operators. Idempotent per
 * pointer: mounting the same binder twice would make it be consulted twice, which is wasted work rather
 * than a wrong answer, but the deduplication costs one comparison and removes the question.
 *
 * @ownership   observes `binder` (the caller keeps ownership)
 * @thread      main
 * @pre         none
 * @post        `binder` appears exactly once in `execution_binders()` afterwards
 * @invariant   Mounting does not reorder what is already there: order is the consultation order
 * @errors      May allocate; allocation failure terminates
 * @complexity  O(n) in the mounted count
 * @nondet      none
 * @frozen      no
 * @tests       views.binders.mounted_once_and_in_order
 */
void mount_execution_binder(qp::graph::execution::IOperatorBinder* binder);

}  // namespace qp::views::model
