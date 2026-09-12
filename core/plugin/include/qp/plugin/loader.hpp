/**
 *              plugin.loader.empty_set_loads_nothing_successfully,
 *              plugin.loader.reports_every_failure_not_just_the_first
 * @file loader.hpp
 * @brief The plugin entry point, and the host side that maps it into this process.
 *
 * ## What this header fixes, and what it deliberately leaves open
 *
 * `manifest.hpp` decides everything that can be judged from a *value*: identity,
 * ABI, capabilities, dependency order. This header is the other half -- the
 * moment a plugin stops being a description and becomes code running inside the
 * host's address space.
 *
 * The boundary is one exported C function. C, not C++, and one function, not a
 * class: the whole point of a plugin ABI is that two binaries built by different
 * compilers on different days can still find each other. A C++ class crossing
 * this line would need the same standard library, the same exception model, the
 * same name mangling, and the same ABI for every type in its signature. Every
 * one of those is a way for the two sides to disagree silently.
 *
 * What the loader does **not** do is decide what a plugin contributes. A kernel
 * wants the kernel registry; a node type wants the IR type registry; an exporter
 * wants the format registry. If this header named any of them, `core/plugin`
 * would grow a dependency edge onto every module that owns a registry, and the
 * next registry anybody adds would be a change to the plugin foundation. So the
 * contribution is an opaque callback and the registries stay with their owners.
 *
 * ## Unloading is the part with teeth
 *
 * A registry holds **non-owning** pointers to objects the plugin owns --
 * `KernelDesc::impl` is the documented case. Dropping the shared library while a
 * registry still points into it leaves every later use reading unmapped memory,
 * and the failure surfaces wherever the kernel is next called, arbitrarily far
 * from the unload that caused it. That is why `unregister` exists as a required
 * part of the contract rather than an oversight, and why `LoadedPlugin` will not
 * release the library until it has been called successfully.
 *
 * @ownership   observes
 * @invariant   Each declaration states its own ownership in more detail
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The exported entry point is a C function with C linkage
 * @errors      See each declaration
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       plugin.loader.loads_and_unloads, plugin.loader.refuses_missing_entry
 */
#pragma once

#include <qp/diag.hpp>
#include <qp/plugin/manifest.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qp::plugin {

/**
 * @brief Tag identifying the plugin ABI this block was compiled against.
 *
 * A struct's first field being a magic value is the cheapest available check that
 * the bytes about to be reinterpreted really are the export block. Without it, a
 * library that happens to export a symbol of the right *name* but the wrong
 * *shape* -- a stale build, a differently-versioned fork -- is reinterpreted as a
 * `PluginExports` and the host reads whatever follows. The failure is not a clean
 * error; it is a call through a pointer that was never a function pointer.
 *
 * Its value spells nothing and means nothing beyond "the same four bytes". It is
 * a coin that both sides bite.
 */
inline constexpr std::uint32_t kExportsTag = 0x5150504Bu; // 'QPPK'

/// @brief ABI version of the export block shape. Bump when PluginExports changes.
///
/// Separate from `kPluginAbiVersion` on purpose. That one tracks the *manifest*
/// schema, which can gain an optional field without breaking anyone. This one
/// tracks the *struct layout* handed across the boundary, where adding a field
/// changes an offset. They change for different reasons and must be free to move
/// independently.
inline constexpr std::uint16_t kExportsVersion = 1;

/**
 * @brief The manifest as it crosses the module boundary: pointers and integers only.
 *
 * ## Why the boundary cannot carry a `Manifest`
 *
 * The first version of this header pointed straight at a `Manifest`, and it was
 * wrong in a way that did not appear until a real shared library was mapped. A
 * `Manifest` holds `std::string` and `std::vector<std::string>`, and those are not
 * data: their layout is decided by whichever standard library and runtime built the
 * plugin. A host that read a `std::string` written by a plugin built against a
 * different runtime read a length from the wrong offset, allocated that many bytes
 * from a heap the plugin does not share, and died on the memcpy. Nothing in the
 * type system objects, because on the host's side the expression is well-typed.
 *
 * So the boundary carries this instead. The host converts it into a `Manifest` on
 * its own side, with its own `std::string`, which is the only side that can safely
 * allocate.
 *
 * ## The strings are borrowed and must be static
 *
 * They live in the plugin's read-only data and stay valid for as long as the
 * library is mapped -- longer than any conversion needs. A plugin that assembled a
 * string at runtime and handed over a pointer to a temporary would break that, which
 * is why the conversion copies immediately and never stores one of these pointers.
 *
 * @ownership   observes
 * @thread      main
 * @pre         `id`, `name`, and every dependency entry are static, non-null,
 *              null-terminated, and readable for the library's lifetime
 * @post        none
 * @invariant   Contains no type with a constructor, destructor, or virtual function
 * @errors      noexcept
 * @frozen      yes -- a layout change must bump kExportsVersion
 * @tests       plugin.loader.loads_and_unloads
 */
struct PluginManifestC final {
    /// Stable machine identity, e.g. "org.example.spring". Not null.
    const char* id = nullptr;
    /// Human-readable name, shown in the plugin list. Not null.
    const char* name = nullptr;
    std::uint16_t version_major = 0;
    std::uint16_t version_minor = 0;
    std::uint16_t version_patch = 0;
    /// Field-buffer layout the plugin was built against. Must equal the host's.
    std::uint16_t layout = 0;
    /// Capability bits, as in `Capability`.
    std::uint32_t capabilities = 0;
    /// Length of `dependencies`. Zero when the plugin has none.
    std::uint32_t dependency_count = 0;
    /// Array of `dependency_count` null-terminated ids, or null when the count is 0.
    const char* const* dependencies = nullptr;
};

/**
 * @brief What a plugin contributes, and how to take it back.
 *
 * @ownership   observes
 * @invariant   The plugin owns everything reachable from here; the host only calls
 * @thread      main
 * @pre         `tag` and `exports_version` were written by this header
 * @post        none
 * @invariant   `manifest` remains valid for as long as the library is loaded
 * @errors      See the callbacks
 * @frozen      yes -- a layout change must bump kExportsVersion
 * @tests       plugin.loader.loads_and_unloads
 */
struct PluginExports final {
    /// Must equal `kExportsTag`. Checked before any other field is read.
    std::uint32_t tag = 0;
    /// Must equal `kExportsVersion`.
    std::uint16_t exports_version = 0;
    /// Reserved, so the struct starts 8-byte aligned and the manifest is not the
    /// first thing a wrong-shape block would hit.
    std::uint16_t reserved = 0;
    /// What this plugin claims, in flat C form. Judged before `register_into` runs.
    const PluginManifestC* manifest = nullptr;

    /**
     * @brief Installs the plugin's contributions into the host's registries.
     *
     * @param host_context Opaque, host-owned, and passed through unchanged. The
     *        plugin knows what it really is; this header does not, which is what
     *        keeps the plugin foundation free of dependency edges onto every
     *        registry-owning module. A plugin that needs no context ignores it.
     * @return `ErrorCode::ok`, or a non-zero code on failure. **A partial
     *         installation must be undoable**: `unregister` is called even when
     *         this reports a failure, so a plugin that installed half of itself
     *         must be able to take that half back.
     *
     * @ownership   value
     * @thread      main
     * @pre         The manifest passed judgement
     * @post        On success, this plugin's contributions are live
     * @invariant   Installs non-owning references; the host's registries hold them
     *              until `unregister` runs, and this is called at most once per load
     * @errors      Any plugin-domain code; the specific choice belongs to the plugin
     * @frozen      yes
     * @tests       plugin.loader.loads_and_unloads,
     *              plugin.loader.failed_registration_leaves_nothing
     */
    diag::ErrorCode (*register_into)(void* host_context) = nullptr;

    /**
     * @brief Removes everything `register_into` installed. Must be idempotent.
     *
     * **Required, not optional**, and this is the whole safety argument for
     * unloading. A registry's pointers into this library stay valid only until the
     * library is released, so the host needs a way to make them stop existing first.
     * A plugin that cannot undo its registration cannot be unloaded -- and an
     * optional callback here would mean half the ecosystem silently cannot be, with
     * the difference invisible until a user removes a plugin at runtime.
     *
     * Idempotent because it is also the cleanup path for a **partial**
     * `register_into`: the host cannot know how far that got, so it calls this
     * unconditionally and the plugin must tolerate it.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none -- valid even if `register_into` was never called, and
     *              receives the same `host_context` that was
     * @post        No pointer into this library remains in any registry
     * @invariant   Releases the plugin's own contributions, and is safe to call more
     *              than once
     * @errors      Reports nothing; a failure to clean up is not actionable, because
     *              the host must treat the library as permanently loaded regardless
     * @frozen      yes
     * @tests       plugin.loader.unload_unregisters_first
     */
    void (*unregister)(void* host_context) = nullptr;
};

/// @brief The symbol name every plugin library must export, with C linkage.
///
/// A single fixed name rather than one per plugin: the host has to find the entry
/// point before it knows anything about the plugin, so the name cannot come from
/// the plugin. It is the one string both sides must agree on.
inline constexpr const char* kEntryPointName = "qp_plugin_entry";

/// @brief The signature of `kEntryPointName`.
///
/// Returns a pointer to a `PluginExports`, or null to refuse to load. Returning
/// null is a legitimate answer: a plugin may find that this host lacks something
/// it needs that no manifest field expresses, and declining is better than
/// installing itself and failing later.
using EntryPointFn = const PluginExports* (*)();

/**
 * @brief Converts a boundary descriptor into a `Manifest` the host can use.
 *
 * Total on any readable descriptor: a null `id`, `name`, or `dependencies` array
 * becomes an empty string or an empty list rather than a crash. That is deliberate.
 * The check that a plugin *has* an identity belongs to `judge`, which reports it as
 * `missing_identity`; doing it here as well would mean a plugin with no name is
 * rejected by a null check somewhere in the loader and never reaches the code whose
 * job is to explain why. A host that cannot name a plugin should still be able to
 * say "this file declared no identity".
 *
 * @ownership   owns
 * @thread      main
 * @pre         none -- valid for a default-constructed descriptor
 * @post        Every string in the result is non-null, possibly empty
 * @invariant   The result is a copy; nothing in it points into the plugin
 * @errors      May allocate; allocation failure terminates, as elsewhere in this project
 * @complexity  O(total string length)
 * @nondet      none
 * @frozen      no
 * @tests       plugin.loader.loads_and_unloads
 */
[[nodiscard]] Manifest to_manifest(const PluginManifestC& c);

/**
 * @brief Why a plugin file could not be brought into the process.
 *
 * Finer-grained than `ErrorCode` because the recovery differs: a missing file is
 * a packaging mistake, a missing symbol is a build mistake, a wrong tag is a
 * version mistake. Collapsing these into `plugin_load_failed` would make the log
 * say "failed" to all three.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `ok` is the only non-failure value
 * @errors      noexcept
 * @frozen      no
 * @tests       plugin.loader.reports_distinct_failures,
 *              plugin.loader.missing_file_is_distinct_from_unmappable,
 *              plugin.loader.refuses_wrong_tag,
 *              plugin.loader.refuses_wrong_exports_version,
 *              plugin.loader.refuses_missing_manifest,
 *              plugin.loader.refuses_missing_unregister,
 *              plugin.loader.refuses_declining_plugin
 */
enum class LoadFailure : std::uint8_t {
    ok = 0,
    /// The path does not name a readable file.
    file_missing = 1,
    /// The OS refused to map it: wrong architecture, missing dependency, corrupt.
    library_unmappable = 2,
    /// Loaded, but does not export `kEntryPointName`.
    entry_point_missing = 3,
    /// The entry point returned null: the plugin declined this host.
    plugin_declined = 4,
    /// The block's `tag` is wrong: these bytes are not a `PluginExports`.
    wrong_tag = 5,
    /// The block's `exports_version` is not one this host can read.
    wrong_exports_version = 6,
    /// The block's `manifest` pointer is null.
    manifest_missing = 7,
    /// `unregister` is null, so the plugin could never be safely unloaded.
    unregister_missing = 8,
    /// `judge` refused the manifest.
    manifest_rejected = 9,
    /// `register_into` reported a failure.
    registration_failed = 10,
};

/// @brief Stable short name for `LoadFailure`, for logs and test ids.
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        One of a fixed set of literals
/// @invariant   Never empty
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
/// @tests       plugin.loader.reports_distinct_failures
[[nodiscard]] const char* to_string(LoadFailure f) noexcept;

/**
 * @brief The outcome of attempting to load one plugin file.
 *
 * Carries the failure **and** the manifest when one was readable. A rejected
 * plugin is exactly the case where the user needs to be told which plugin it was
 * -- reporting "a plugin failed to load" without the identity it declared leaves
 * the reader to bisect their own plugin directory.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        `manifest` is populated whenever the file yielded a readable one,
 *              including when `failure != ok`
 * @invariant   `failure == ok` implies `plugin` is non-null in the owning report
 * @errors      noexcept
 * @frozen      no
 * @tests       plugin.loader.refuses_incompatible_manifest
 */
struct LoadOutcome final {
    LoadFailure failure = LoadFailure::ok;
    /// Path as given by the caller, kept for the message.
    std::string path{};
    /// The manifest the file declared, when one could be read.
    Manifest manifest{};
    /// Whether `manifest` came from the file. False means "no manifest available",
    /// which is different from "a manifest saying nothing".
    bool has_manifest = false;
    /// Number of contributions installed. Zero on any failure.
    std::size_t contributions = 0;

    /// @brief Whether the plugin was installed and is live.
    [[nodiscard]] bool ok() const noexcept { return failure == LoadFailure::ok; }
};

/**
 * @brief A mapped plugin library, and the only object that can release it.
 *
 * Move-only, and it is the destructor that matters: releasing a library whose
 * registry entries are still live is the hazard described at the top of this
 * file. `unload()` therefore calls `unregister` first and only then releases the
 * mapping, and the destructor goes through the same path.
 *
 * @ownership   owns
 * @invariant   Holds the OS library handle and the exports block's address space
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The library is mapped for exactly the lifetime of this object
 * @errors      See unload()
 * @frozen      no
 * @tests       plugin.loader.loads_and_unloads, plugin.loader.unload_unregisters_first,
 *              plugin.loader.moves_transfer_ownership_once,
 *              plugin.loader.move_assignment_releases_the_old_library
 */
class LoadedPlugin final {
public:
    /// @brief An object holding no library. `valid()` is false.
    LoadedPlugin() noexcept = default;
    ~LoadedPlugin();

    LoadedPlugin(const LoadedPlugin&) = delete;
    LoadedPlugin& operator=(const LoadedPlugin&) = delete;
    LoadedPlugin(LoadedPlugin&& other) noexcept;
    LoadedPlugin& operator=(LoadedPlugin&& other) noexcept;

    /// @brief Whether a library is currently mapped.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   True for every object returned by `load_plugin`
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       plugin.loader.loads_and_unloads
    [[nodiscard]] bool valid() const noexcept;

    /// @brief The manifest the plugin declared, valid while the library is mapped.
    ///
    /// @ownership   borrows from the loaded library
    /// @thread      main
    /// @pre         valid()
    /// @post        none
    /// @invariant   Identical on every call
    /// @errors      Undefined if `valid()` is false
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       plugin.loader.loads_and_unloads
    [[nodiscard]] const Manifest& manifest() const noexcept;

    /// @brief The path this plugin was loaded from.
    ///
    /// @ownership   borrows from this object
    /// @thread      main
    /// @pre         valid()
    /// @post        none
    /// @invariant   As passed to `load_plugin`
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       plugin.loader.loads_and_unloads
    [[nodiscard]] const std::string& path() const noexcept;

    /// @brief Whether this plugin's contributions are currently installed.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        False after `unload()`
    /// @invariant   A plugin that never installed anything reports false
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       plugin.loader.loads_and_unloads
    [[nodiscard]] bool contributes() const noexcept;

    /**
     * @brief Calls the plugin's `register_into` with `host_context`.
     *
     * Kept separate from `load_plugin` because installation order is derived from
     * the manifests, and the manifests only exist once every library is mapped. It
     * is public rather than private because a single-plugin host is a legitimate
     * caller, and that caller should go through the same accounting as the bulk path
     * instead of reaching for the exports block itself.
     *
     * @ownership   value
     * @invariant   Installs non-owning references into host registries
     * @thread      main
     * @pre         `valid()`
     * @post        `contributes()` is true on success, unchanged on failure
     * @invariant   Idempotent: installing an already-installed plugin returns ok
     * @errors      noexcept; the plugin's own failure is returned as an ErrorCode,
     *              never thrown, and a plugin that reports failure has `unregister`
     *              called on it before this returns
     * @complexity  O(cost of the plugin's registration)
     * @nondet      Runs plugin code
     * @frozen      no
     * @tests       plugin.loader.loads_and_unloads,
     *              plugin.loader.failed_registration_leaves_nothing
     */
    [[nodiscard]] diag::ErrorCode install(void* host_context) noexcept;

    /**
     * @brief Unregisters the plugin's contributions and releases the library.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        `valid()` and `contributes()` are false
     * @invariant   Releases the plugin's own contributions, and is idempotent:
     *              unloading an already-unloaded object does nothing
     * @errors      noexcept -- once a plugin is loaded, refusing to unload it is
     *              not an available answer, because the alternative is leaking the
     *              mapping for the life of the process
     * @complexity  O(cost of the plugin's unregistration)
     * @nondet      Runs plugin code
     * @frozen      no
     * @tests       plugin.loader.unload_unregisters_first
     */
    void unload() noexcept;

private:
    friend LoadOutcome load_plugin(const std::string&, void*, LoadedPlugin*);

    // The OS handle, or nullptr. Void rather than a platform type so this header
    // stays includable on every platform and pulls in no system header.
    void* handle_ = nullptr;
    const PluginExports* exports_ = nullptr;
    /// The host's own copy of the manifest, converted from the plugin's flat
    /// descriptor at load time. Owned rather than borrowed, for two reasons: the
    /// plugin's descriptor cannot hold a `std::string` at all, and a manifest that
    /// outlived its library would otherwise point into unmapped memory. Copying here
    /// is what lets `manifest()` return a reference valid for this object's life.
    Manifest manifest_{};
    /// The context passed to `install`, kept because `unregister` must receive the
    /// **same** one. Storing it here is what makes `unload()` take no arguments,
    /// which in turn is what lets the destructor be correct on every path.
    void* host_context_ = nullptr;
    bool registered_ = false;
    std::string path_{};
};

/**
 * @brief The result of loading a set of plugin files.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `plugins.size()` equals the number of files that loaded
 * @errors      noexcept
 * @frozen      no
 * @tests       plugin.loader.load_set_is_ordered,
 *              plugin.loader.load_set_is_all_or_nothing,
 *              plugin.loader.empty_set_loads_nothing_successfully,
 *              plugin.loader.reports_every_failure_not_just_the_first
 */
struct LoadReport final {
    /// Successfully loaded plugins, in the order they were loaded.
    std::vector<LoadedPlugin> plugins{};
    /// One entry per input path, in the input order, including successes.
    std::vector<LoadOutcome> outcomes{};
    /// How many plugins the caller asked for. Loading is all-or-nothing, so this
    /// equals `plugins.size()` whenever `all_ok()` is true.
    std::size_t requested = 0;

    /// @brief Whether every requested plugin loaded.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   Equivalent to every outcome having `failure == ok`
    /// @errors      noexcept
    /// @complexity  O(outcomes.size())
    /// @nondet      none
    /// @frozen      no
    /// @tests       plugin.loader.load_set_is_all_or_nothing
    [[nodiscard]] bool all_ok() const noexcept;

    /// @brief The first failure, or nullptr when every plugin loaded.
    ///
    /// @ownership   borrows from this object
    /// @thread      main
    /// @pre         none
    /// @post        null when `all_ok()`
    /// @invariant   Reports the first failure in input order, not an arbitrary one
    /// @errors      noexcept
    /// @complexity  O(outcomes.size())
    /// @nondet      none
    /// @frozen      no
    /// @tests       plugin.loader.load_set_is_all_or_nothing
    [[nodiscard]] const LoadOutcome* first_failure() const noexcept;
};

/**
 * @brief Maps one plugin file, judges it, and installs its contributions.
 *
 * The order is not an implementation detail. The manifest is judged **before**
 * `register_into` is called, because the alternative -- install first, check
 * after -- means an incompatible plugin has already written into the registries
 * by the time anyone notices, and undoing that is a best-effort operation against
 * a plugin that is by definition not behaving as this host expects. The whole
 * value of judging a manifest is that it happens before anything else does.
 *
 * @param path          A shared library exporting `kEntryPointName`.
 * @param host_context  Opaque, passed through to the plugin's `register_into`.
 *                      May be null when the plugin's registries need no context.
 * @param out           Receives the loaded plugin on success. Must be non-null.
 *
 * @ownership   owns
 * @invariant   On success, `*out` owns the mapped library
 * @pre         `out != nullptr`
 * @post        On failure `*out` is untouched and no registry was modified
 * @invariant   A returned `ok` outcome means the library is mapped and live
 * @errors      Never throws; every failure is a `LoadFailure` in the result
 * @complexity  O(size of the library)
 * @nondet      Depends on the filesystem and on the plugin's own code
 * @frozen      no
 * @tests       plugin.loader.loads_and_unloads, plugin.loader.refuses_missing_entry,
 *              plugin.loader.reports_distinct_failures,
 *              plugin.loader.refuses_incompatible_manifest
 */
[[nodiscard]] LoadOutcome load_plugin(const std::string& path, void* host_context,
                                      LoadedPlugin* out);

/**
 * @brief Loads a set of plugins in dependency order, or loads none of them.
 *
 * ## Why all-or-nothing
 *
 * A partially loaded plugin set is the failure mode this platform exists to avoid
 * elsewhere, in a different costume. If plugin B depends on plugin A, and A fails,
 * then B's registrations may be missing, and a graph built against B produces
 * numbers that are wrong rather than absent -- the user sees a simulation run and
 * finish. Refusing the whole set turns a wrong answer into no answer, which the
 * user can act on.
 *
 * The cost is real and worth naming: one broken plugin prevents a session from
 * starting. That is why the report carries **every** failure rather than stopping
 * at the first -- the user fixes their plugin directory in one pass instead of one
 * rebuild per plugin.
 *
 * @param paths         Shared libraries to load. Order is irrelevant; it is
 *                      recomputed from the manifests.
 * @param host_context  Opaque, passed through to each plugin's `register_into`.
 *
 * @ownership   owns
 * @invariant   The returned report owns every loaded library
 * @thread      main
 * @pre         none
 * @post        Either every path loaded, or `plugins` is empty and every failure
 *              is recorded in `outcomes`
 * @invariant   The returned plugins are in dependency order
 * @errors      Never throws
 * @complexity  O(n log n) plus the cost of mapping each library
 * @nondet      Depends on the filesystem and on the plugins' own code
 * @frozen      no
 * @tests       plugin.loader.load_set_is_ordered,
 *              plugin.loader.load_set_is_all_or_nothing
 */
[[nodiscard]] LoadReport load_plugins(const std::vector<std::string>& paths, void* host_context);

}  // namespace qp::plugin
