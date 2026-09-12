# core/graph: the graph and execution layer.
#
# The directory structure has two levels, "layer -> module":
#   core/graph/ir/        intermediate representation (IR) -- the plugin API itself
#   core/graph/structure/ graph structure: connect/disconnect, version, acyclicity
#   core/graph/mutate/    command bus: the only entry point for every mutation + undo
#   core/graph/validate/  load-time validation: types, dimensions, units
#   core/graph/eval/      evaluation scheduling + content-addressed cache
#   core/graph/domain/    the three domain semantics: field / particle / render
#   core/graph/kernels/   native operator contract (implemented in plugins)
#   core/graph/field/     minimal contract for field semantics

add_subdirectory(ir)
add_subdirectory(structure)
add_subdirectory(mutate)
add_subdirectory(validate)
add_subdirectory(eval)
add_subdirectory(domain)
add_subdirectory(field)
add_subdirectory(kernels)
add_subdirectory(execution)
