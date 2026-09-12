# plugins/analysis: regression and uncertainty propagation.
#
# **The first producer and first consumer of `runtime/store`'s `FitResult`.** That type has been frozen since the
# store was written -- coefficients, a full covariance matrix, residuals, chi-squared, degrees of freedom,
# R-squared -- and until this directory existed, nothing in the repository produced one and nothing read one. The
# plan tree recorded it as a gap with an explicit reopening condition, and the condition was "the first real
# demand". A straight line through a set of measurements is the most common piece of analysis in a first-year lab,
# so the demand was never hypothetical, only unserved.
#
# That nothing in `core/` changed to accommodate this module is the evidence for the framework's own claim: a new
# **category** of content -- analysis, as opposed to models, instruments or formats -- arrived as content, filling
# an interface that already existed.
#
# Why not a generic "analysis" framework with a registry of algorithms: there is exactly one algorithm family
# here (weighted linear least squares) and one propagation toolkit, both of which are pure functions from numbers
# to numbers. A registry would need a plugin boundary, a capability bit and a version rule to serve **zero**
# callers that want them, which is the "declared but unconnected" shape this repository keeps finding and
# removing. `plugins/analysis` is a library that `qp_test_plugins_analysis` and a future node type call directly.
#
# Nothing here includes a Qt header: a fit has to run in a headless regression test and inside a report generator.
#
# Dependency rule: plugins may depend on core, core may never depend on plugins.

add_library(qp_plugins_analysis STATIC
    src/fit.cpp
    src/propagate.cpp)

add_library(qp::plugins::analysis ALIAS qp_plugins_analysis)

target_include_directories(qp_plugins_analysis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_plugins_analysis PUBLIC cxx_std_20)

target_link_libraries(qp_plugins_analysis
    PUBLIC qp::runtime_store qp::diag
    PRIVATE qp_warnings qp_sanitize)
