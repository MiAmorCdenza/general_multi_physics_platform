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
    # The value conversion every text format needs, one layer below the formats themselves: it decides what a
    # parameter *is*, and a format decides how a file spells it. See its CMakeLists for why it is not inside
    # `qpjson`.
    add_subdirectory(formats/value_text)
    add_subdirectory(mechanics)
    # The measuring devices. They are `instruments/` rather than part of `mechanics/` because the plan tree
    # splits content by what a thing **is**: a caliper is not a rigid-body model, and the one interface they
    # share is `runtime/instrument`'s, which no other content implements.
    add_subdirectory(instruments)

    # Regression and uncertainty propagation. It has no interface of its own -- it is a library of pure
    # functions over `runtime/store`'s types, and the `FitResult` it produces is the one `store` has always
    # declared. See its CMakeLists for why it is not a registry of algorithms.
    add_subdirectory(analysis)

    # The physics models: a damped oscillator, a pendulum without the small-angle approximation, and a
    # projectile with linear drag. They are `models/` and not more entries in `mechanics/` because what they own
    # is a **node type and its parameters**, which is a different kind of contribution from an integrator.
    add_subdirectory(models)

    # The worked experiments, and the closed forms they are checked against. Last, because it consumes the
    # other three: an example is a model, driven by the loop, with a reading taken by an instrument and a fit
    # run over the results. See its CMakeLists for why examples are code rather than saved documents.
    add_subdirectory(examples)

    # A teacher's experiment file: the graph, its name, and the inputs a student may turn. It is **content**
    # rather than a format directory because the meaning it adds is not syntax -- see its CMakeLists. It is also
    # the second document format consumer, and it composes with the first rather than copying it, which is what
    # section 9.9 of the plan tree said could not be done without extracting the marshalling first.
    add_subdirectory(experiments)
endif()
