/**
 * @file main.cpp
 * @brief The shell executable: opens the editor.
 *
 * A thin `main`, and deliberately so. Everything with behaviour lives in
 * `qp::views` so that it can be constructed and inspected without an event loop;
 * an application whose logic exists only inside `main` cannot be tested at all,
 * and the first casualty is always the thing that was hard to reach.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   No application state is created outside the window
 * @errors      returns the Qt event loop's exit code
 * @frozen      no
 * @tests       views.editor_window.shares_one_session
 */
#include "editor_window.hpp"

#include <qp/views/model/execution_binders.hpp>

#if defined(QP_HAS_MECHANICS_PLUGIN)
#include <qp/plugins/mechanics/mechanics_binder.hpp>
#endif

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // The one place that knows which plugins exist. Mounting happens before the window is built, because
    // the window's run controller captures the list during construction.
    //
    // Guarded by `QP_BUILD_PLUGINS`: with plugins off there is nothing to mount, and the Run action then
    // reports that no node has an operator -- which is the accurate description of that build.
#if defined(QP_HAS_MECHANICS_PLUGIN)
    qp::plugins::mechanics::MechanicsBinder mechanics;
    qp::views::model::mount_execution_binder(&mechanics);
#endif

    qp::views::EditorWindow window;
    // The application decides to seed itself; the window does not. One call, so the graph and
    // the measurement session cannot be seeded in the wrong order by a caller that only
    // remembered one of them.
    window.seed_demo();
    window.show();
    return QApplication::exec();
}
