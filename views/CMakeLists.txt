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
    qt/confidence_panel.cpp
    qt/confidence_panel.hpp
    qt/fit_panel.cpp
    qt/fit_panel.hpp
    qt/editor_window.cpp
    qt/editor_window.hpp
    qt/scene_view.cpp
    qt/scene_view.hpp
    qt/theme.cpp
    qt/theme.hpp
    qt/icons.cpp
    qt/icons.hpp
    # The icon sheet, compiled into the library by AUTORCC (on at the top of this file) and linked into every
    # consumer. A plain `.qrc` source rather than `qt_add_resources`, for two reasons: the bitmap table in
    # `qt/icons.cpp` is the source of truth and this only packages what `scripts/make_icon_sheet.py` rasterised
    # from it, so no Qt-provided build step is wanted; and `qt_add_resources` protects its derived variables
    # from escaping a directory scope, which is friction a STATIC library does not need.
    #
    # In the **library** rather than in the shell, so `qp_test_views_theme` links the same bytes the application
    # ships. A resource compiled into an executable is invisible to a test that lives in another target, and
    # "the shipped sheet matches the table" would then be a claim about a file no test could open.
    qt/qp_icons.qrc
    src/hello_view.cpp
    include/qp/views/hello_view.hpp)

add_library(qp::views ALIAS qp_views)

target_compile_definitions(qp_views PUBLIC QT_NO_KEYWORDS)

target_include_directories(qp_views
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_views PUBLIC cxx_std_20)
# `qp::host` is here because the window borrows the composition root: the catalog a content plugin registers
# into is the host's, and a window that owned its own would be the second answer to "which types exist" that
# this layer already had to remove once. Linking it into the view library rather than only into the shell is
# what lets `tests/unit/views/` construct a window at all.
target_link_libraries(qp_views
    PUBLIC Qt6::Widgets
           qp::views_model
           qp::host
           qp::authoring_commands qp::authoring_document qp::authoring_capability
           qp::runtime_run qp::runtime_store
           qp::graph_kernels qp::field
    PRIVATE qp_warnings qp_sanitize)

# The fit, and the one place a **view** links a plugin.
#
# `views/model/fit_session` decides which readings may enter a fit and is built unconditionally; the fit itself is
# `plugins/analysis`, so the call sits in `views/qt/fit_panel.cpp` behind this definition. The alternative --
# linking the Qt-free half of the view layer against a plugin -- would make a plugins-off build fail to configure,
# and `check_core_standalone.py` exists to keep that configuration real.
#
# Guarded on the target rather than on the option, like every other plugin link in this project: the two can
# disagree, and the target set is what the linker will actually see.
if(TARGET qp::plugins::analysis)
    target_link_libraries(qp_views PRIVATE qp::plugins::analysis)
    # PUBLIC, not PRIVATE, and the difference is not cosmetic: the definition decides whether the **test** that
    # asserts a coefficient arrives with its uncertainty compiles that assertion at all. A PRIVATE definition left
    # the test in the branch for a build with no fitter, and that branch passes -- so the case reported success
    # while asserting nothing about a fit. A green test that cannot fail is worse than a missing one.
    target_compile_definitions(qp_views PUBLIC QP_HAS_ANALYSIS_PLUGIN)
endif()

# The shell: the editor wired to the demonstrator library. Still not the finished
# application, but it now exercises the canvas and the property panel rather than
# merely proving that a Qt process can link core.
add_executable(qp_shell
    app/main.cpp)

target_include_directories(qp_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/qt)
# `qp::host` is the composition root, and the shell is the one caller that is allowed to know it: the host
# owns the node catalog that content plugins register into, and the window borrows that catalog. Linking it
# here rather than into `qp_views` is the dependency direction -- a window that linked the host would be a
# window that could load plugins by itself, which is not a decision a widget should be able to make.
target_link_libraries(qp_shell PRIVATE qp::views qp::host)

# The application is where plugin knowledge belongs, and the macro is how it knows whether there is
# any. Defined here rather than in a header so the window cannot accidentally depend on it: `views`
# must build with plugins absent, and a macro it could see would invite exactly that.
#
# Four mounts, because the application is the only place allowed to know which plugins exist: the operators a run
# needs, the format a document is saved as, the format a trace is exported as, and the devices a reading can be
# taken with. The window consults four lists it did not fill.
if(QP_BUILD_PLUGINS)
    target_compile_definitions(qp_shell PRIVATE QP_HAS_MECHANICS_PLUGIN)
    target_link_libraries(qp_shell PRIVATE qp::plugins::mechanics)

    target_compile_definitions(qp_shell PRIVATE QP_HAS_FORMAT_PLUGINS)
    target_link_libraries(qp_shell PRIVATE qp::plugins::qpjson qp::plugins::csv)

    # The measuring devices, registered through the host's built-in path. Linked here rather than left to the
    # plugin directory, so the device list is filled whether or not anything was loaded from disk -- which is the
    # difference between "the platform can measure" and "the platform can measure once somebody ships a plugin".
    target_compile_definitions(qp_shell PRIVATE QP_HAS_INSTRUMENTS_PLUGIN)
    target_link_libraries(qp_shell PRIVATE qp::plugins::instruments)

    # The teaching-experiment bundle. Linked here for the same reason as the instruments: a teacher's file must
    # open whether or not a plugin directory exists beside the executable. It composes with `qpjson` rather than
    # replacing it -- see `main.cpp` for why the format is constructed over the document format instead of
    # beside it -- so this definition and `QP_HAS_FORMAT_PLUGINS` are both required at the call site.
    target_compile_definitions(qp_shell PRIVATE QP_HAS_EXPERIMENT_PLUGIN)
    target_link_libraries(qp_shell PRIVATE qp::plugins::experiments)

    # The magnetosphere kit: a field model, an emitter and a pusher, three node types the palette can show and a
    # graph can be drawn from. Linked here for the same reason as the instruments -- the types must be in the
    # catalog whether or not anything was loaded from disk, or the kit is reachable only from a test fixture.
    #
    # **This comment used to say the kit could not be run, and that was wrong.** It claimed that "pressing Run on a
    # magnetosphere graph says so", because `views/model`'s run controller drives `IStateOperator`s and this kit's
    # particles live in a `ParticleState` -- and the second run path had in fact been written: the controller asks
    # the mounted `IGraphRunProvider`s after the operator path declines, `run.hpp` states the run's own cadence, and
    # `main.cpp` mounts the provider four lines above the node types. What is below is the correction, measured by
    # doing what a user does: a new document, `Demos -> magnetosphere`, Run, and the window reports
    # **"boris: 4096 steps, 24 of 24 live"** with the particles drawn in one scene and the traced field lines in the
    # other (`nodes 16 | edges 21 | graph v154`).
    #
    # The lesson is not the mount but the comment: a claim about a *capability* is the one kind of comment that
    # becomes false without anybody editing it, and this one was used to justify a design decision.
    # `run.controller.a_content_graph_runs_through_the_provider` now asserts the path -- including that the same
    # graph is *refused* when the provider is not mounted -- so the sentence cannot rot again unnoticed.
    target_compile_definitions(qp_shell PRIVATE QP_HAS_MAGNETOSPHERE_PLUGIN)
    target_link_libraries(qp_shell PRIVATE qp::plugins::magnetosphere)
endif()
