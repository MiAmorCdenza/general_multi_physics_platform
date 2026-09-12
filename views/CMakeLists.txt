# views/: the Qt user interface. A **consumer**, deletable as a whole.
#
# Iron rule (standards/enforcement.md section 4): core must not depend on this
# directory, and this directory may depend on core. `QP_BUILD_VIEWS=OFF` must
# leave core building and passing every test -- that is what makes the GUI an
# optional front end rather than the application.
#
# Two halves, and the split is what makes the layer testable:
#
#   model/  Qt-FREE. The enumerable node catalog, the built-in demonstrator
#           library, and the rule that decides which control edits a port. Plain
#           C++ over core types, covered by the ordinary Catch2 suite on BOTH
#           compilers -- including GCC, which cannot link Qt at all.
#   qt/     Needs a toolkit: painting a canvas, building widgets.
#
# The editor-selection rule lives in model/ deliberately. It is the part most
# likely to be subtly wrong -- a bounded control where none was declared silently
# rejects legitimate values -- and a rule inside a QWidget can only be tested with
# a Qt test harness and an event loop.
#
# Qt is found only here. Nothing under core/ may include a Qt header, and the layer
# gate (scripts/check_layers.py, rule L2) enforces it rather than a code review.
#
# Widgets, not QML: see docs/adr/ADR-0006-gui-framework-and-qt-licensing.md. This is
# a measurement instrument, the authoring views need a real text editor as much as
# a canvas, and QML would add a second runtime to debug for animation features a
# lab bench does not have.

# Qt is found by the root CMakeLists, so that its imported targets are visible to
# tests/ as well -- CTest needs to know where Qt's DLLs live in order to launch a
# GUI test.
#
# The model half is added **unconditionally** by the root too. It contains no Qt,
# so gating it behind QP_BUILD_VIEWS would be a lie about what it needs, and it
# would hide the view layer's most testable logic from the configuration that runs
# on both compilers. Only the Qt half in this directory is conditional.

# AUTOMOC must be on **before** any target in this directory is created: the
# variable is read when add_library/add_executable runs, so setting it afterwards
# silently leaves that target without a code generator. That happened once here --
# qp_shell linked with three unresolved `HelloView::metaObject` symbols because
# only qp_views had been created after the flag was set.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# `slots`, `signals`, `emit` and `foreach` are **preprocessor macros** in Qt, not
# keywords. Any core header using one of those words as an identifier is rewritten
# before the compiler parses it -- and core has `Graph::slots()`, so this is not
# hypothetical:
#
#     graph.hpp(411): error C2059: syntax error: '<parameter-list>'
#
# The fix is to turn the macros off rather than to reorder includes. Reordering
# would leave the collision armed for whichever header comes first next, and the
# error it produces points at a core header that is perfectly correct. With
# QT_NO_KEYWORDS the explicit spellings are Q_SLOTS / Q_SIGNALS / Q_EMIT.
#
# The headers are listed as sources on purpose. AUTOMOC discovers a Q_OBJECT by
# scanning the sources **of a target**, so a header that only arrives through
# include propagation gets its moc output in whichever target happens to include
# it -- which is how qp_shell once failed to link while the moc file sat unused in
# its own autogen directory.
add_library(qp_views STATIC
    qt/node_graph_view.cpp
    qt/node_graph_view.hpp
    qt/property_panel.cpp
    qt/property_panel.hpp
    qt/measurement_panel.cpp
    qt/measurement_panel.hpp
    qt/editor_window.cpp
    qt/editor_window.hpp
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
           qp::views_model
           qp::authoring_commands qp::authoring_document qp::authoring_capability
           qp::runtime_run qp::runtime_store
           qp::graph_kernels qp::field
    PRIVATE qp_warnings qp_sanitize)

# The shell: the editor wired to the demonstrator library. Still not the finished
# application, but it now exercises the canvas and the property panel rather than
# merely proving that a Qt process can link core.
add_executable(qp_shell
    app/main.cpp)

target_include_directories(qp_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/qt)
target_link_libraries(qp_shell PRIVATE qp::views)
