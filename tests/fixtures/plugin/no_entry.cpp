/**
 * @file no_entry.cpp
 * @brief A library that exports a plausible-looking symbol with the wrong name.
 *
 * The distinction from `declines.cpp` is the point of having both. A missing symbol
 * is a build mistake -- the plugin was never finished, or was built with a
 * misspelled entry point -- while a null return is a decision the plugin made about
 * this host. One is fixed by rebuilding, the other by changing what the host offers,
 * and reporting them as the same thing sends the reader to the wrong place.
 */
#if defined(_WIN32)
#  define QP_EXPORT extern "C" __declspec(dllexport)
#else
#  define QP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Misspelled on purpose: `qp_plugin_entrypoint` rather than `qp_plugin_entry`. A
// library exporting nothing at all would trip the same check, but this is the
// realistic mistake and it also proves the lookup matches the exact name rather
// than a prefix.
QP_EXPORT const void* qp_plugin_entrypoint() { return nullptr; }
