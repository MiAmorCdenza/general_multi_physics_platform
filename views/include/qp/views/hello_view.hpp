/**
 * @file hello_view.hpp
 * @brief The minimal window that proves the view layer is wired to the core.
 *
 * ## Why this exists before any real panel
 *
 * Building a Qt GUI carries exactly one kind of risk that no amount of design
 * removes: whether Qt can be found, whether moc runs, whether the core links into
 * a process that also links Qt. That risk is independent of what the panels look
 * like, and it is much cheaper to retire now than to discover while debugging a
 * node editor.
 *
 * So this window does the smallest thing that proves the chain, and it prints the
 * core's state rather than a placeholder **on purpose**: a demo that showed
 * hard-coded text would pass while the link to the core was broken. Everything it
 * displays comes from a real `authoring::Session` and a real `runtime::RunLedger`.
 *
 * It is not the application. It is the proof that the application can exist.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   Every displayed number is read from the core, never hard-coded
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.shell.links_core_state
 */
#pragma once

#include <QWidget>

#include <qp/authoring/commands/session.hpp>
#include <qp/runtime/run/run.hpp>

namespace qp::views {

/**
 * @brief A window that reports what the core actually contains.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   The session outlives the widget's use of it
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.shell.links_core_state
 */
class HelloView final : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief Builds the window and seeds the session with one demonstrator node.
     *
     * The node is added through `authoring::Session::apply`, which is the only
     * mutation path a view is allowed to use. Doing it here rather than through a
     * direct graph edit is deliberate: it exercises the rule the whole view layer
     * depends on -- one graph, one command bus, one undo stack -- from the first
     * line of UI code, so a violation shows up immediately rather than after
     * three panels have each grown their own copy.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         none
     * @post        The window shows the session's node count and the ledger's run count
     * @invariant   No state is stored outside the session and the ledger
     * @errors      May allocate while building the window; failure to allocate
     *              terminates rather than being reported, because a window that
     *              cannot be built has nowhere to report to
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.shell.links_core_state
     */
    explicit HelloView(QWidget* parent = nullptr);

    /// @brief The session this window displays. A view holds the session, never a graph.
    [[nodiscard]] qp::authoring::Session& session() noexcept { return session_; }

    /// @brief The ledger this window displays.
    [[nodiscard]] qp::runtime::RunLedger& ledger() noexcept { return ledger_; }

private:
    qp::authoring::Session session_{};
    qp::runtime::RunLedger ledger_{};
};

}  // namespace qp::views
