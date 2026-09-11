# views/: the Qt user interface. A **consumer**, deletable as a whole.
#
# Iron rule (standards/enforcement.md section 4): core must not depend on this
# directory, and this directory may depend on core. `QP_BUILD_VIEWS=OFF` must
# leave core building and passing every test -- that is what makes the GUI an
# optional front end rather than the application.
#
# Qt is found only here. Nothing under core/ may include a Qt header, and the
# layer gate (scripts/check_layers.py, rule L2) is what enforces it rather than a
# code review.
#
# Widgets, not QML: see docs/adr/ADR-0006-gui-framework-and-qt-licensing.md. The
# short version is that this is a measurement instrument, the authoring views need
# a real text editor as much as a canvas, and QML would add a second runtime to
# debug for animation features a lab bench does not have.

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)

# AUTOMOC must be on **before** any target in this directory is created: the
# variable is read when add_library/add_executable runs, so setting it afterwards
# silently leaves that target without a code generator. That is exactly what
# happened here -- qp_shell linked with three unresolved `HelloView::metaObject`,
# `qt_metacast` and `qt_metacall` symbols, because only qp_views had been created
# after the flag was set.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)


# `slots`, `signals`, `emit` and `foreach` are **preprocessor macros** in Qt, not
# keywords. Any core header that uses one of those words as an identifier is then
# rewritten before the compiler parses it -- and core has `Graph::slots()`, so this
# is not hypothetical: it produced
#
#     graph.hpp(411): error C2059: syntax error: '<parameter-list>'
#
# because `slots()` became `QT_ANNOTATE_FUNCTION(qt_slots)`.
#
# The fix is to turn the macros off for this target rather than to reorder
# includes. Reordering would leave the collision armed for whichever header is
# included first next, and the error it produces points at a core header that is
# perfectly correct. With QT_NO_KEYWORDS the explicit spellings are `Q_SLOTS`,
# `Q_SIGNALS` and `Q_EMIT`, which cost nothing and cannot collide with English
# identifiers a core module might reasonably choose.
# The header is listed as a source on purpose. AUTOMOC discovers a Q_OBJECT by
# scanning the sources **of a target**, so a header that only arrives through
# include propagation produces moc output in whichever target happens to include
# it -- here qp_shell, which then failed to link with three unresolved
# `HelloView::metaObject` symbols while the moc file sat unused in qp_shell's
# autogen directory. Listing it here makes qp_views the target that owns it.
add_library(qp_views STATIC
    src/hello_view.cpp
    include/qp/views/hello_view.hpp)

add_library(qp::views ALIAS qp_views)

target_compile_definitions(qp_views PUBLIC QT_NO_KEYWORDS)

target_include_directories(qp_views
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_views PUBLIC cxx_std_20)
target_link_libraries(qp_views
    PUBLIC Qt6::Widgets
           qp::authoring_commands qp::authoring_document qp::authoring_capability
           qp::runtime_run qp::runtime_store
           qp::graph_kernels qp::field
    PRIVATE qp_warnings qp_sanitize)

# The one executable in this phase. It exists to prove the whole chain works --
# Qt found, moc run, core linked, a window shown -- which is the part of a GUI
# decision that carries real risk. The real panels come next.
add_executable(qp_shell
    app/main.cpp)

target_link_libraries(qp_shell PRIVATE qp::views)
