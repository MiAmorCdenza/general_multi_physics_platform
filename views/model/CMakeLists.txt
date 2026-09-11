# views/model: the view layer's Qt-FREE half.
#
# This split is not tidiness, it is what makes the view layer testable.
#
# Everything here is plain C++ over core types: the enumerable node-type catalog,
# the built-in demonstrator library, and the rule that decides which control edits
# a port. None of it needs a QApplication, an event loop or a widget, so it is
# covered by the ordinary Catch2 suite that already runs on both compilers --
# including the GCC build, which cannot link Qt at all.
#
# The Qt half (views/qt) is the part that genuinely needs a toolkit: painting a
# canvas and building widgets. Putting the editor-selection rule over there would
# have meant it could only be tested with a Qt test harness, and it is precisely
# the part most likely to be subtly wrong -- a bounded control where none was
# declared rejects legitimate values, and a numeric control for a text type
# displays a 0 where the user's path used to be.
#
# Note the include path: qp/views/model/... rather than qp/views/qt/..., because a
# Qt-free component whose header says "qt" misleads the next reader.

add_library(qp_views_model STATIC
    type_catalog.cpp
    demo_library.cpp
    editor_choice.cpp)

add_library(qp::views_model ALIAS qp_views_model)

target_include_directories(qp_views_model
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_views_model PUBLIC cxx_std_20)
target_link_libraries(qp_views_model
    PUBLIC qp::graph_ir qp::ports qp::units qp::authoring_portui
    PRIVATE qp_warnings qp_sanitize)
