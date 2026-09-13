/**
 * @file run_providers.hpp
 * @brief Where the window gets its whole-graph run providers from.
 *
 * The companion of `execution_binders.hpp`, one level up and for the same reason: a provider lives in the plugin
 * that knows how to run its content, and a window that named that plugin by type could not be built without it.
 * So the view layer declares **where providers come from** and the application -- the one place allowed to know
 * which plugins exist -- fills the list.
 *
 * An empty list is a legitimate state: a build with no particle kits has none, and a graph of that kind then gets
 * the operator loop's own answer, which names the domain the node belongs to.
 *
 * @ownership   mixed -- see each declaration
 * @thread      main (filled once during startup, read on every run)
 * @pre         none
 * @post        none
 * @invariant   The returned reference stays valid until the process ends
 * @errors      noexcept
 * @frozen      no
 * @tests       views.binders.mounted_once_and_in_order
 */
#pragma once

#include <qp/graph/execution/run_provider.hpp>

#include <vector>

namespace qp::views::model {

/**
 * @brief The run providers the application mounted, in the order they should be consulted.
 *
 * @ownership   borrows (the list holds pointers; whoever registered a provider owns it)
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
[[nodiscard]] const std::vector<qp::graph::execution::IGraphRunProvider*>& run_providers() noexcept;

/**
 * @brief Adds a provider to the list the window consults.
 *
 * @param provider The provider. Borrowed; it must outlive the window.
 *
 * @ownership   observes `provider`
 * @thread      main
 * @pre         `provider` outlives the window
 * @post        `provider` is the last entry
 * @invariant   A null provider is not added
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       views.binders.mounted_once_and_in_order
 */
void mount_run_provider(qp::graph::execution::IGraphRunProvider* provider) noexcept;

/**
 * @brief Forgets every provider, for a test that wants a clean list.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        `run_providers()` is empty
 * @invariant   No provider is destroyed: the list only borrows
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       views.binders.mounted_once_and_in_order
 */
void clear_run_providers() noexcept;

}  // namespace qp::views::model
