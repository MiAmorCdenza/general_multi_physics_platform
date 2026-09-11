# core/authoring: view services -- the layer that lets several views share one
# editing session.
#
#   5.1 document/    graph document open/save, layout metadata slotted by view id
#   5.2 capability/  plugins declare capabilities, the host negotiates
#   5.3 commands/    graph/mutate's command bus exposed to views, one undo stack
#   5.4 layout/      layout algorithm contract (the algorithms are plugins)
#   5.5 portui/      UI descriptions driven by the port type registry
#
# The split is deliberate and load order matters: capability depends only on
# plugin, and portui depends on capability. Adding a subdirectory here therefore
# never creates a cycle back into graph.

add_subdirectory(capability)
add_subdirectory(commands)
