# plugins/: content. Everything under this directory can be deleted.
#
# That sentence is a promise the repository makes and a test enforces, so it is
# worth being precise about what it means here. `core/` must build and pass its
# whole suite with this directory removed; the layer gate refuses any include from
# core into plugins, and `tests/unit/smoke/test_core_standalone.cpp` is built
# without them.
#
# Content is split the way the plan tree splits it, by what a thing *is* rather than
# by which experiment uses it:
#
#   mechanics/    integrators and rigid-body models
#   analysis/     regression, uncertainty propagation, chi-square, residuals
#   instruments/  concrete instruments, including their error models
#   models/       field models: dipole, magnetotail, envelopes
#   formats/      document and data formats (JSON today, a CSV trace exporter next)
#   examples/     example graphs and documents
#
# There is deliberately no `kernels/` split separate from a domain. A Boris pusher
# is a kernel *and* a plasma model, and a directory tree that forces it into one of
# two names would be a taxonomy nobody consults after the first week.
#
# Building this directory is OFF by default. Core must configure with no plugin
# present, and a plugin that fails to compile should not stop someone working on the
# foundation -- the same reasoning that keeps Qt behind QP_BUILD_VIEWS.

if(QP_BUILD_PLUGINS)
    add_subdirectory(formats)
    add_subdirectory(mechanics)
endif()
