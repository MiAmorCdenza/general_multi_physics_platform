# core/runtime/store: the shape of measured data.
#
# Dependency rule: store depends on units / diag / ports
# (see plan-tree.md section 4.2 and the ALLOWED table in scripts/check_layers.py).
#
# The decision this module encodes, and the reason it is foundation rather than a
# plugin: **the contract is foundation, the algorithms are plugins.** Regression,
# uncertainty propagation, chi-squared and residual analysis are all content
# (`plugins/analysis/`). A platform that shipped its own least-squares would have
# decided, for every course in every university, what a fit means.
#
# What the foundation owns instead is the shape: `Measurement`, `Dataset`,
# `FitResult`. Without it every plugin invents its own way to carry an
# uncertainty, a regression written by one author cannot consume a series
# produced by another, and the lab ends up back in a spreadsheet -- which is the
# competitor this platform is actually measured against.
#
# The three statistics implemented here (mean, sample standard deviation,
# standard error) are present because they are definitional rather than
# methodological, and because charter R3 makes the Type-A uncertainty of the mean
# a platform promise rather than an analysis choice. Everything beyond them is a
# plugin.

add_library(qp_runtime_store STATIC
    src/store.cpp)

add_library(qp::runtime_store ALIAS qp_runtime_store)

target_include_directories(qp_runtime_store
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_runtime_store PUBLIC cxx_std_20)
target_link_libraries(qp_runtime_store
    PUBLIC qp::units qp::diag qp::ports
    PRIVATE qp_warnings qp_sanitize)
