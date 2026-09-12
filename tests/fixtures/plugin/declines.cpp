/**
 * @file declines.cpp
 * @brief An entry point that returns null: the plugin refuses this host.
 *
 * Distinct from `no_entry.cpp` because the causes differ. A missing symbol means the
 * library cannot participate at all; a null return means it can, but something it
 * needs is absent -- a capability no manifest field expresses, a service this build
 * does not provide. Declining is better than installing itself and failing later,
 * because at load time the host can still say which plugin declined and why.
 */
#include <cstdint>

#if defined(_WIN32)
#  define QP_EXPORT extern "C" __declspec(dllexport)
#else
#  define QP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Returns null by decision, not by mistake.
QP_EXPORT const void* qp_plugin_entry() { return nullptr; }
