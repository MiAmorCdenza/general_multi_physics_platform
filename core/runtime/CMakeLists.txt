# core/runtime: runs and data -- the layer the previous project did not have.
#
#   4.1 run/    run identity: seed + parameters + graph version + plugin versions
#   4.2 store/  dataset / measurement / uncertainty / fit result structures
#   4.3 trace/  timeline, snapshots, reading series
#   4.4 io/     format adapters (the formats themselves are plugins)
#   4.5 file/   whole-file I/O and the one UTF-8 path conversion
#   4.6 instrument/  the contract a measuring device answers (C6)
#
# The plan tree calls this layer out explicitly: the old abstraction had "a graph
# + a version number + slots" and no concept of "one experiment run". Without it
# the whole traceability chain -- charter R2, C1 and C3 -- has nowhere to live.

add_subdirectory(run)
add_subdirectory(store)
add_subdirectory(trace)
add_subdirectory(file)
add_subdirectory(instrument)
add_subdirectory(io)
