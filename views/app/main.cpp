/**
 * @file main.cpp
 * @brief The shell executable: opens the wiring-check window.
 *
 * A thin `main`, and deliberately so. Everything with behaviour lives in
 * `qp::views` so that it can be constructed and inspected by a test; an
 * application whose logic exists only inside `main` cannot be tested at all, and
 * the first casualty is always the thing that was hard to reach.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   No application state is created outside the window
 * @errors      returns the Qt event loop's exit code
 * @frozen      no
 * @tests       views.shell.links_core_state
 */
#include <qp/views/hello_view.hpp>

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    qp::views::HelloView window;
    window.show();
    return QApplication::exec();
}
