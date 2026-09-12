# plugins/experiments: a teacher's experiment file, and the second format consumer.
#
# **Why this is not `plugins/formats/qpx`.** A `.qpd` document and a `.qpx` experiment are different things, and
# the difference is not the syntax. A document is the graph a user built; an experiment is the graph a teacher
# handed out **and the list of what a student may change in it**. The second has a meaning the first does not, and
# a directory named after syntax would be holding a concept. `formats/` holds the things whose whole content is
# what the bytes look like.
#
# **What this plugin is evidence for.** `docs/plan-tree.md` section 9.9 recorded the second format's cost as "the
# marshalling of nodes, edges, parameters and layouts, which `qpjson` does in both directions", and the reopening
# condition as "extract that marshalling first, then a second format is days of work". This plugin is the
# measurement of that claim, and the answer is that the extraction was not needed: an experiment file carries a
# whole `.qpd` as **one nested member**, and `QpJsonFormat` parses it. So the ~450 lines of graph marshalling
# exist once, in the format that already had them, and the two formats cannot drift because there is only one
# implementation of the part that could.
#
# That is the same shape as `plugins/formats/value_text`'s conclusion one level down -- one conversion, two
# consumers -- arrived at from the other direction: there, a shared piece was extracted because both formats
# needed the *same* computation; here, nothing was extracted because one format can simply **call** the other.
#
# Nothing here includes a Qt header, and nothing here knows a document's shape: the nested member is handed to the
# delegate as text.
#
# Dependency rule: plugins may depend on core, core may never depend on plugins.

add_library(qp_plugins_experiments STATIC
    src/experiments.cpp)

add_library(qp::plugins::experiments ALIAS qp_plugins_experiments)

target_include_directories(qp_plugins_experiments
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_plugins_experiments PUBLIC cxx_std_20)

# `qp::authoring_persist` is PUBLIC because the public header implements its interface: a caller that linked only
# the runtime would fail to compile on a base class it cannot see. `qp::graph_ir` is PUBLIC for `INodeCatalog`,
# which the constructor takes -- the catalog is how a parameter learns its label, unit and bounds, and a plugin
# that hard-coded those would be a second place for a node type to be described.
#
# Note what is **not** here: `qp::plugins::qpjson`. The document format arrives as an `IDocumentFormat&` at
# construction, so this plugin depends on the interface and not on the implementation -- which is what makes the
# composition root free to hand it any format, and what keeps the two formats from being one thing.
target_link_libraries(qp_plugins_experiments
    PUBLIC qp::authoring_persist qp::graph_ir qp::graph_structure qp::units qp::diag
    PRIVATE qp_warnings qp_sanitize)