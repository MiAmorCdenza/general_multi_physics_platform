/**
 * @file host.hpp
 * @brief The composition root: load plugin libraries, negotiate what they may do, mount what they contribute.
 *
 * ## The hole this fills
 *
 * `core/plugin` maps a shared library and judges its manifest, and its own header says what it deliberately
 * does **not** do: "decide what a plugin contributes", because naming the kernel registry, the instrument
 * registry or the format registry would grow a dependency edge onto every module that owns one. That boundary
 * is right and it left a hole. Nothing in the repository ever called `load_plugins`, and a content plugin had
 * no way in. The hole was invisible because the application links its content statically -- `mount_*` calls in
 * `main` -- so "plugins are content the host mounts" described a build, not the platform.
 *
 * This module is the other half. It is the **only** module allowed to depend on every registry at once, and
 * the dependency direction is the point: nothing depends on `host` except the application, so adding a
 * registry is a change here rather than a change to the plugin foundation.
 *
 * ## Two interfaces, and why the split is not ceremony
 *
 * `IPluginHost` is what a plugin sees. It is the **plugin ABI surface**: a plugin casts the opaque
 * `host_context` to this type, so adding or reordering a virtual here changes what a plugin compiled against
 * the old one may call. The manifest already carries the tag that refuses such a plugin -- `PluginManifestC::
 * layout` -- and this is the reason that field exists.
 *
 * `PluginHost` is what the application sees: it owns the registries, loads and unloads, and reports.
 *
 * ## Every registration is recorded, which is what makes unload honest
 *
 * A registry holds **non-owning** pointers to objects the plugin owns, so unloading means the container must
 * be emptied *before* the library is released. The plugin's own `unregister` is supposed to do that, and it is
 * not sufficient on its own: `LoadedPlugin::unload` releases the library whether or not `unregister` managed
 * to clean up, so a plugin that forgets one node type leaves a dangling descriptor behind and the failure
 * surfaces wherever that type is next read, arbitrarily far from the unload that caused it.
 *
 * So the host does not trust the callback to be complete. Every `add_*` here records what it added, and
 * `unload` removes the recorded items itself before the library is released. The plugin's `unregister` still
 * runs -- it may own resources no registry knows about -- but it is no longer the only thing standing between
 * a session and a dangling pointer.
 *
 * The record also answers a question nothing else could: **which plugin contributed this?**. `origin_of` reads
 * it for the node palette and the plugin list. A registry alone cannot answer it, because `NodeDesc::source`
 * is a string the plugin chose and a plugin can get its own name wrong.
 *
 * ## What is gated, and what a gate here is not
 *
 * A manifest declares capability **bits**, and the host compares them with its grant before `register_into`
 * runs. That is a declaration check, not a sandbox. The interface is one object rather than a per-capability
 * facade, so a plugin that declared `kernels` may still call `add_node_type`. Stated plainly because the
 * alternative is a reader assuming isolation that is not there: an in-process plugin is trusted code, and the
 * gate exists to catch a **mistake** -- a manifest that does not match what the code does -- not to contain a
 * hostile library.
 *
 * The finer, string-keyed mechanism in `authoring/capability` is composed here rather than left unused: the
 * host registers each mounted plugin as a provider whose `needs` are the bits its manifest declared, and the
 * grant is this host's. That is what closes the gap where a plugin's own account of itself decided its own
 * permission.
 *
 * @ownership   owns (the mount table, the capability registry and every content registry)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A plugin id is mounted at most once
 * @errors      Reports through `diag::Result` and `LoadReport`; never throws
 * @frozen      no
 * @tests       host.loads_a_real_plugin_and_mounts_what_it_registers,
 *              host.refuses_a_capability_it_did_not_grant
 */
#pragma once

#include <qp/authoring/capability/capability.hpp>
#include <qp/diag/result.hpp>
#include <qp/graph/ir/node_type_registry.hpp>
#include <qp/graph/kernels/registry.hpp>
#include <qp/plugin/loader.hpp>
#include <qp/runtime/instrument/instrument.hpp>
#include <qp/runtime/io/io.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Qt defines `slots`, `signals`, `emit` and `foreach` as preprocessor macros. A host built inside a Qt program
// would otherwise have every one of those words rewritten before the compiler saw it, and `views/CMakeLists.txt`
// carries the same guard for the same reason: a header that uses one as an identifier fails with an error that
// points at a correct line. Reported rather than silently worked around.
#if defined(slots) || defined(signals) || defined(emit) || defined(foreach)
#  error "qp/host/host.hpp requires QT_NO_KEYWORDS: Qt's keyword macros collide with this code"
#endif

namespace qp::host {

/**
 * @brief What a mounting plugin is handed, and the whole of this module's plugin-facing surface.
 *
 * The plugin casts the opaque `host_context` to this type. Every method returns `diag::ErrorCode` rather than a
 * richer type on purpose: an error code is an integer, so it crosses the boundary without either side having
 * to agree on a string's layout. The reason a plugin wants to give a user travels through `refuse` instead,
 * which takes a `const char*` the plugin owns and the host copies -- the conversion happens on the host's
 * side, with the host's allocator, for the same reason `to_manifest` exists.
 *
 * @ownership   observes (the host owns every registry)
 * @thread      main
 * @pre         The host outlives the plugin's use of this interface
 * @post        none
 * @invariant   The same host returns the same registry objects on every call
 * @errors      noexcept; every failure is a returned `ErrorCode`
 * @frozen      yes -- a method added, removed or reordered changes the plugin ABI
 * @tests       host.loads_a_real_plugin_and_mounts_what_it_registers
 */
class IPluginHost {
public:
    IPluginHost() = default;
    virtual ~IPluginHost() = default;
    IPluginHost(const IPluginHost&) = delete;
    IPluginHost& operator=(const IPluginHost&) = delete;

    /**
     * @brief The id from the manifest of the plugin currently registering, or empty outside a registration.
     *
     * A plugin needs this for one thing: filling `NodeDesc::source` so a node can be attributed. The host does
     * not use that field -- it keeps its own record -- but a user reading a node's properties should not have
     * to look anything up.
     *
     * @ownership   borrows (points into this host's mount table; valid until that plugin is unloaded)
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   Identical for every call made during one `register_into`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       host.loads_a_real_plugin_and_mounts_what_it_registers
     */
    [[nodiscard]] virtual std::string_view mounting_plugin() const noexcept = 0;

    /**
     * @brief Whether this host granted `bit` to the plugin that is registering.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   Constant for the lifetime of the host
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       host.refuses_a_capability_it_did_not_grant
     */
    [[nodiscard]] virtual bool granted(plugin::Capability bit) const noexcept = 0;

    /**
     * @brief The ids of the plugins already mounted, in mount order.
     *
     * A plugin needs this for the one question a manifest cannot answer for it: "is my dependency actually
     * installed". A manifest declares dependency **ids** and the loader orders by them, but ordering is not
     * presence -- a plugin whose dependency failed elsewhere is asked to install into a host that is missing
     * it, and its own registration would then produce a graph that runs and is wrong.
     *
     * Returned by value as views rather than as a reference, because the list changes and a borrowed container
     * would be invalidated by the next load.
     *
     * @ownership   borrows the ids (they live in this host's mount table)
     * @thread      main
     * @pre         none
     * @post        One entry per mounted plugin, in mount order
     * @invariant   Excludes the plugin being mounted, which is not yet in the table
     * @errors      noexcept
     * @complexity  O(plugins)
     * @nondet      none
     * @frozen      no
     * @tests       host.mounts_a_dependent_after_its_dependency
     */
    [[nodiscard]] virtual std::vector<std::string_view> mounted_plugins() const noexcept = 0;

    /**
     * @brief Adds a node type to the host's catalog.
     *
     * @ownership   owns (the descriptor is moved into the registry)
     * @thread      main
     * @pre         none
     * @post        On success the type is in the catalog, attributed to this plugin
     * @invariant   On failure nothing was added
     * @errors      noexcept; `invalid_argument` for an unusable description, `duplicate_connection` for a name
     *              this host already serves
     * @complexity  O(types)
     * @nondet      none
     * @frozen      no
     * @tests       host.loads_a_real_plugin_and_mounts_what_it_registers
     */
    [[nodiscard]] virtual diag::ErrorCode add_node_type(graph::NodeDesc desc) noexcept = 0;

    /**
     * @brief Adds a time-stepping kernel.
     *
     * @ownership   observes (the implementation stays owned by the plugin)
     * @thread      main
     * @pre         `desc.impl` is non-null and `desc.name` is non-empty
     * @post        On success the kernel is findable by name
     * @invariant   On failure nothing was added
     * @errors      noexcept; `invalid_argument` for an unusable description, `duplicate_connection` for a name
     *              already served
     * @complexity  O(kernels)
     * @nondet      none
     * @frozen      no
     * @tests       host.loads_a_real_plugin_and_mounts_what_it_registers
     */
    [[nodiscard]] virtual diag::ErrorCode add_kernel(const graph::kernels::KernelDesc& desc) noexcept = 0;

    /**
     * @brief Adds a measuring device.
     *
     * @ownership   observes (the device stays owned by the plugin, and must outlive the registration)
     * @thread      main
     * @pre         `instrument` is non-null and its description has an id
     * @post        On success the device is findable by id
     * @invariant   On failure nothing was added
     * @errors      noexcept; `invalid_argument` for a null device or an empty id, `duplicate_connection` for an
     *              id already served
     * @complexity  O(devices)
     * @nondet      none
     * @frozen      no
     * @tests       host.loads_a_real_plugin_and_mounts_what_it_registers
     */
    [[nodiscard]] virtual diag::ErrorCode add_instrument(runtime::IInstrument* instrument) noexcept = 0;

    /**
     * @brief Adds an export format.
     *
     * @ownership   observes (the exporter stays owned by the plugin)
     * @thread      main
     * @pre         `exporter` is non-null and its format has a name and an extension
     * @post        On success the exporter is findable by name and by extension
     * @invariant   On failure nothing was added
     * @errors      noexcept; `invalid_argument` for a null exporter or an unusable format,
     *              `duplicate_connection` for a name or extension already served
     * @complexity  O(formats)
     * @nondet      none
     * @frozen      no
     * @tests       host.loads_a_real_plugin_and_mounts_what_it_registers
     */
    [[nodiscard]] virtual diag::ErrorCode add_exporter(runtime::IExporter* exporter) noexcept = 0;

    /**
     * @brief Records why this plugin is giving up, and returns the code to return from `register_into`.
     *
     * The host cannot invent this sentence. A plugin refuses for its own reasons -- "this build has no BLAS",
     * "the hardware is absent" -- and a host that reported only `plugin_fault` would leave the user reading a
     * code that names the symptom and not the cause. Passing the string as a borrowed `const char*` keeps the
     * allocation on the host's side.
     *
     * @param reason A static, null-terminated sentence. Null is treated as "no reason given".
     *
     * @ownership   observes (`reason` must outlive the call; a string literal is the intended use)
     * @thread      main
     * @pre         none
     * @post        The next `LoadReport` entry for this plugin carries this sentence
     * @invariant   The last call before returning wins
     * @errors      noexcept
     * @complexity  O(length of reason)
     * @nondet      none
     * @frozen      no
     * @tests       host.reports_a_plugins_own_reason_for_refusing
     */
    [[nodiscard]] virtual diag::ErrorCode refuse(const char* reason) noexcept = 0;
};

/**
 * @brief One plugin this host tried to mount, and what became of it.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `path` is filled for every attempt, mounted or refused
 * @errors      noexcept
 * @frozen      no
 * @tests       host.reports_every_failure_with_a_reason
 */
struct PluginOutcome final {
    /// The manifest id when one could be read, otherwise the file name.
    std::string id{};
    /// The human-readable name from the manifest, or the id when there was none.
    std::string name{};
    /// Where the attempt was made.
    std::string path{};
    /// Whether the plugin is now mounted: mapped, judged, negotiated with, and installed.
    bool mounted = false;
    /// The code, for the log.
    diag::ErrorCode code = diag::ErrorCode::ok;
    /// What to tell a user: a sentence naming this plugin, not a bare code.
    std::string detail{};
    /// How many contributions the host recorded for it. Zero is a legitimate answer -- a plugin that exists
    /// only to be depended on -- and it is reported rather than hidden, because "it loaded" and "it did
    /// something" are different claims.
    std::size_t contributions = 0;

    /// @brief Whether the plugin is live.
    [[nodiscard]] bool ok() const noexcept { return mounted; }
};

/**
 * @brief What one load attempt or one directory scan produced.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Every attempt appears exactly once, in `mounted_plugins` or in `refused`
 * @errors      noexcept
 * @frozen      no
 * @tests       host.reports_every_failure_with_a_reason
 */
struct LoadReport final {
    /// The plugins that mounted, in the order they were installed.
    std::vector<PluginOutcome> mounted_plugins{};
    /// The plugins that did not, each with a reason, in the order they were tried.
    std::vector<PluginOutcome> refused{};

    /**
     * @brief Whether every attempt succeeded. True for an empty report.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   Equivalent to `refused.empty()`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       host.reports_every_failure_with_a_reason
     */
    [[nodiscard]] bool all_mounted() const noexcept { return refused.empty(); }

    /**
     * @brief How many plugins were attempted.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   The sum of both lists
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       host.reports_every_failure_with_a_reason
     */
    [[nodiscard]] std::size_t attempts() const noexcept {
        return mounted_plugins.size() + refused.size();
    }

    /**
     * @brief One line per attempt, for a log or a status line.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        Empty for an empty report; otherwise one line per attempt and a trailing newline
     * @invariant   Names every refusal's reason
     * @errors      May allocate
     * @complexity  O(attempts)
     * @nondet      none
     * @frozen      no
     * @tests       host.reports_every_failure_with_a_reason
     */
    [[nodiscard]] std::string to_text() const;
};

/**
 * @brief The host: it owns the content registries, mounts plugins into them, and can take them back out.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Every mounted plugin's recorded contributions are removed by `unload`
 * @errors      See each declaration
 * @frozen      no
 * @tests       host.loads_a_real_plugin_and_mounts_what_it_registers,
 *              host.unload_takes_the_contributions_back
 */
class PluginHost final : public IPluginHost {
public:
    /**
     * @brief A host that will grant `grant` to the plugins it mounts.
     *
     * @param grant The capability bits this host is willing to grant. A plugin whose manifest declares a bit
     *              outside it is refused **before** `register_into` runs, because a contribution the host
     *              never agreed to cannot be made to look like an accident afterwards.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   The grant never widens
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       host.refuses_a_capability_it_did_not_grant
     */
    explicit PluginHost(plugin::Capability grant) noexcept : grant_(grant) {}

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;
    ~PluginHost() override;

    /**
     * @brief The capability bits this host grants.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   Same value as passed to the constructor
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       host.refuses_a_capability_it_did_not_grant
     */
    [[nodiscard]] plugin::Capability grant() const noexcept { return grant_; }

    [[nodiscard]] std::string_view mounting_plugin() const noexcept override;
    [[nodiscard]] bool granted(plugin::Capability bit) const noexcept override;
    [[nodiscard]] std::vector<std::string_view> mounted_plugins() const noexcept override;
    [[nodiscard]] diag::ErrorCode add_node_type(graph::NodeDesc desc) noexcept override;
    [[nodiscard]] diag::ErrorCode add_kernel(const graph::kernels::KernelDesc& desc) noexcept override;
    [[nodiscard]] diag::ErrorCode add_instrument(runtime::IInstrument* instrument) noexcept override;
    [[nodiscard]] diag::ErrorCode add_exporter(runtime::IExporter* exporter) noexcept override;
    [[nodiscard]] diag::ErrorCode refuse(const char* reason) noexcept override;

    // -- What the application reads. Const, so a caller cannot register around the record. --

    /// @brief The node types, for a palette or a lookup.
    [[nodiscard]] const graph::NodeTypeRegistry& node_types() const noexcept { return node_types_; }
    /// @brief The kernels, for an operator list.
    [[nodiscard]] const graph::kernels::KernelRegistry& kernels() const noexcept { return kernels_; }
    /// @brief The instruments, for a device list.
    [[nodiscard]] const runtime::InstrumentRegistry& instruments() const noexcept { return instruments_; }
    /// @brief The export formats, for a "save as" list.
    [[nodiscard]] const runtime::FormatRegistry& formats() const noexcept { return formats_; }
    /// @brief The capability registry, for "who offers this".
    [[nodiscard]] const authoring::Registry& capabilities() const noexcept { return capabilities_; }

    /// @brief The id the ledger attributes a built-in node type to.
    ///
    /// Not a plugin id and deliberately not shaped like one: `qp.builtin` can never be a manifest id, because a
    /// manifest id is a reverse-domain name a distributor controls. A ledger entry that could be mistaken for a
    /// plugin's would make "which plugin contributed this" answerable with the wrong plugin.
    static constexpr std::string_view kBuiltinOrigin = "qp.builtin";

    /**
     * @brief Adds a node type that this build ships rather than loads.
     *
     * The editor has a handful of demonstrator types -- a source, a spring-damper, a readout, an export, a
     * filter -- and they are **not** the platform's physics. What they are is the minimum that exercises every
     * editor path, which is why they exist at all, and the reason this method exists beside `IPluginHost` is
     * that the alternative was worse: a window that owned a second catalog would be a second answer to "which
     * types exist", and the palette would show whichever one it happened to hold. That duplicate was removed
     * once already when content plugins needed somewhere to put a node type.
     *
     * Built-ins go through the same record as everything else, so `unload` would take them back and `origin_of`
     * names them. They are not a mounted plugin, so they do not appear in `mounted_ids()`.
     *
     * @param desc The description. Same rules and the same refusals as a plugin's contribution.
     *
     * @ownership   owns (the descriptor is moved into the catalog)
     * @thread      main
     * @pre         none
     * @post        On success the type is in the catalog, attributed to `kBuiltinOrigin`
     * @invariant   On failure nothing was added
     * @errors      noexcept; `invalid_argument` for an unusable description, `duplicate_connection` for a name
     *              already served
     * @complexity  O(types)
     * @nondet      none
     * @frozen      no
     * @tests       host.a_builtin_type_is_attributed_and_removable
     */
    [[nodiscard]] diag::ErrorCode add_builtin_node_type(graph::NodeDesc desc) noexcept;

    /**
     * @brief Removes every built-in node type, for a session that wants a clean catalog.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        No type attributed to `kBuiltinOrigin` remains registered
     * @invariant   No plugin's contribution is touched
     * @errors      noexcept
     * @complexity  O(built-ins)
     * @nondet      none
     * @frozen      no
     * @tests       host.a_builtin_type_is_attributed_and_removable
     */
    void clear_builtin_node_types() noexcept;

    /**
     * @brief Adds a measuring device that this build ships rather than loads.
     *
     * The device counterpart of `add_builtin_node_type`, and it exists for the same reason one level up: a
     * build whose only instruments arrive through `add_instrument` from a plugin has **no** instrument until a
     * plugin is mounted, so the measurement loop -- the one thing this platform does that a spreadsheet plus a
     * simulator does not -- is unreachable in a build with no plugins loaded. That was the state of this
     * repository before this method existed: `runtime/instrument` had a full contract, a registry, a fault
     * barrier and thirteen test cases, and **nothing in production ever registered a device**.
     *
     * A device registered here is attributed to `kBuiltinOrigin` and appears in the same ledger, so `origin_of`
     * answers for it and `clear_builtin_instruments` takes it back. It is not a mounted plugin, so it does not
     * appear in `mounted_ids()`.
     *
     * The host does **not** own the instrument: `InstrumentRegistry` holds non-owning pointers, exactly as it
     * does for a plugin's device, so the caller keeps the object alive for as long as the host is used. A
     * built-in whose storage was a temporary would leave the registry pointing at freed memory the first time
     * somebody took a reading.
     *
     * @param instrument The device. Borrowed, and must be non-null.
     *
     * @ownership   observes `instrument`
     * @thread      main
     * @pre         `instrument` outlives this host, or is removed first
     * @post        On success `instruments().find(id)` returns it and `origin_of(id)` names `kBuiltinOrigin`
     * @invariant   On failure the registry is unchanged
     * @errors      noexcept; `invalid_argument` for a null device or an empty id, `duplicate_connection` for an
     *              id already served
     * @complexity  O(devices)
     * @nondet      none
     * @frozen      no
     * @tests       host.a_builtin_instrument_is_attributed_and_removable
     */
    [[nodiscard]] diag::ErrorCode add_builtin_instrument(runtime::IInstrument* instrument) noexcept;

    /**
     * @brief Removes every built-in measuring device.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        No device attributed to `kBuiltinOrigin` remains registered
     * @invariant   No plugin's device is touched
     * @errors      noexcept
     * @complexity  O(devices)
     * @nondet      none
     * @frozen      no
     * @tests       host.a_builtin_instrument_is_attributed_and_removable
     */
    void clear_builtin_instruments() noexcept;

    /**
     * @brief Which plugin contributed `name`, or an empty view when nothing did.
     *
     * The answer comes from the host's own record, not from `NodeDesc::source`: a plugin that misspelled its
     * own id would otherwise be believed.
     *
     * `name` is a **node type name or a measuring device's id**, and the two share one lookup because they share
     * one question: "where did this come from". A device that a graph can name -- the readout node a reading is
     * attributed to -- needs exactly the answer a node type needs, and two lookups would be two records of the
     * same contribution.
     *
     * @ownership   borrows from this object
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   Empty exactly when no mounted plugin and no built-in registered that name
     * @errors      noexcept
     * @complexity  O(mounted plugins)
     * @nondet      none
     * @frozen      no
     * @tests       host.loads_a_real_plugin_and_mounts_what_it_registers,
     *              host.a_builtin_instrument_is_attributed_and_removable
     */
    [[nodiscard]] std::string_view origin_of(std::string_view name) const noexcept;

    /**
     * @brief Loads one plugin and lets it install its contributions.
     *
     * The order is not an implementation detail and each step answers a different question:
     *
     *   1. the loader maps the library and judges the tag, the exports version and the manifest -- "is this a
     *      plugin this build can even talk to";
     *   2. the host compares the manifest's declared bits with `grant()` -- "did I agree to what it wants to
     *      do";
     *   3. `register_into` runs with this host as the context -- "put your contributions where they go";
     *   4. the host records what arrived and registers the plugin as a capability provider.
     *
     * A refusal at any step unloads the library and removes whatever the plugin managed to install, so a
     * refused plugin leaves nothing behind. A half-mounted plugin -- types without kernels, a device without a
     * node type -- is the state that produces a graph which runs and is wrong.
     *
     * @param path The library to load.
     *
     * @ownership   owns the mounted plugin
     * @thread      main
     * @pre         none
     * @post        On success the plugin's contributions are in this host's registries and `unload` can take
     *              them back
     * @invariant   On failure no registry changed in what concerns this plugin
     * @errors      Never throws; a refusal is a sentence in the returned outcome
     * @complexity  O(1) plus the plugin's own registration
     * @nondet      only through the filesystem
     * @frozen      no
     * @tests       host.loads_a_real_plugin_and_mounts_what_it_registers,
     *              host.refuses_a_capability_it_did_not_grant,
     *              host.reports_a_plugins_own_reason_for_refusing,
     *              host.reports_every_failure_with_a_reason
     */
    [[nodiscard]] PluginOutcome load(const std::string& path);

    /**
     * @brief Loads every plugin in a directory, in name order.
     *
     * Name order rather than directory order, because a listing is not sorted and two machines would
     * otherwise mount the same plugins in a different order -- which shows up as a different type winning a
     * duplicate, or a different palette order, on two machines looking at one document.
     *
     * The set is **all or nothing**, matching `plugin::load_plugins` and for the same reason: with a
     * dependency missing, a graph built against it produces wrong numbers rather than absent ones, and the
     * user watches a simulation run to completion. One broken plugin therefore prevents the whole directory
     * from mounting, which is why the report carries every failure rather than stopping at the first -- the
     * user fixes the directory in one pass.
     *
     * @param directory The directory to scan. A directory that does not exist yields an empty report rather
     *                  than an error: a build with no plugins is a legitimate build, and `Run` already says
     *                  what that means.
     * @param extension The file extension to consider, without a dot. Empty means "every file".
     *
     * @ownership   owns the mounted plugins
     * @thread      main
     * @pre         none
     * @post        Either every candidate mounted, or none did and every failure is in `refused()`
     * @invariant   The order of attempts is the sorted order of the paths
     * @errors      Never throws
     * @complexity  O(files log files) plus the plugins' own registration
     * @nondet      only through the filesystem
     * @frozen      no
     * @tests       host.scanning_a_directory_mounts_what_it_can_and_reports_the_rest,
     *              host.mounts_and_refuses_the_loaders_own_fixtures
     */
    [[nodiscard]] LoadReport load_directory(const std::string& directory,
                                            std::string_view extension = "");

    /**
     * @brief Unloads a plugin by id: removes what it contributed, then releases the library.
     *
     * The recorded contributions are removed **first**. The plugin's `unregister` then runs inside
     * `LoadedPlugin::unload`, and only then is the mapping released -- so a plugin that forgot to remove a
     * node type no longer leaves a descriptor pointing into unmapped memory.
     *
     * @param plugin_id The manifest id.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        On success the id is no longer mounted, its entries are gone from every registry, and the
     *              library is unmapped
     * @invariant   A plugin that was never mounted is reported as unknown rather than silently ignored
     * @errors      `unknown_node` when no such plugin is mounted
     * @complexity  O(plugins + contributions)
     * @nondet      Runs plugin code, through `unregister`
     * @frozen      no
     * @tests       host.unload_takes_the_contributions_back
     */
    [[nodiscard]] diag::Result<void> unload(std::string_view plugin_id);

    /**
     * @brief The ids of the mounted plugins, in mount order.
     *
     * @ownership   pure (returns copies)
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   The order plugins were installed in
     * @errors      May allocate
     * @complexity  O(plugins)
     * @nondet      none
     * @frozen      no
     * @tests       host.scanning_a_directory_mounts_what_it_can_and_reports_the_rest
     */
    [[nodiscard]] std::vector<std::string> mounted_ids() const;

private:
    /// What one plugin registered, so that unloading it is exact rather than hopeful.
    ///
    /// Capability registrations are absent on purpose: `authoring::Registry` is keyed by plugin id, so
    /// `forget` removes them, and a second list of the same ids could disagree with the first.
    struct Ledger final {
        std::vector<std::string> node_types{};
        std::vector<graph::kernels::KernelId> kernels{};
        std::vector<std::string> instruments{};
        std::vector<std::string> formats{};
        std::size_t total = 0;
    };

    struct Mounted final {
        plugin::LoadedPlugin library{};
        std::string id{};
        /// The plugin's own sentence for a refusal, or empty. Copied on the host's side.
        std::string refusal{};
        Ledger ledger{};
    };

    /**
     * @brief The provider the host registers on a plugin's behalf.
     *
     * It exists because `PluginExports` carries capability *bits* and two callbacks, not an
     * `ICapabilityProvider` -- so a finely negotiated capability is one thing a dynamically loaded plugin
     * cannot currently offer. Rather than paper over that, the host synthesises the provider from the manifest
     * it already judged: the offers are the names of the declared bits, and `needs` is the declaration itself.
     * The grant is then the host's, which is the property that matters -- a plugin's own account of itself
     * never widens its own permission.
     *
     * `id_` is owned rather than borrowed: the mount table's strings live in a `std::vector` that reallocates,
     * and a `string_view` into it would dangle the first time a second plugin mounted.
     */
    class ManifestProvider final : public authoring::ICapabilityProvider {
    public:
        ManifestProvider(std::string id, plugin::Capability declared, const PluginHost* host)
            : id_(std::move(id)), declared_(declared), host_(host) {}

        [[nodiscard]] std::string_view plugin_id() const noexcept override { return id_; }
        [[nodiscard]] std::vector<authoring::CapabilityId> offers() const noexcept override;
        [[nodiscard]] plugin::Capability needs() const noexcept override { return declared_; }

        /// @brief The host this provider speaks for. Used by `offers()` to name the bits this host serves.
        [[nodiscard]] const PluginHost* host() const noexcept { return host_; }

    private:
        std::string id_;
        plugin::Capability declared_;
        const PluginHost* host_;
    };

    /// @brief Refreshes the borrowed view of the mount table that `mounted_plugins()` hands out.
    ///
    /// Called after every change to `mounted_`, so the view a plugin reads while registering is the table as it
    /// stands at that moment. Borrowed rather than owned because the ids themselves live in the table.
    void refresh_mounted_view();

    /// The `Mounted` entry whose registration is running, or null. Set for the duration of `install`.
    Mounted* registering_ = nullptr;

    /// @brief Whether `plugin_id` is already in the mount table.
    [[nodiscard]] bool is_mounted(std::string_view plugin_id) const noexcept;

    /// @brief Drops the capability provider registered for `plugin_id`, if any.
    void forget(const std::string& plugin_id) noexcept;

    /// @brief Removes everything in `ledger` from the registries, in reverse order of contribution.
    void withdraw(const Ledger& ledger) noexcept;

    /// @brief Withdraws an entry's record and forgets its provider, leaving the library mapped.
    void withdraw(Mounted& entry) noexcept;

    /**
     * @brief Runs a mapped plugin's registration and negotiates its capabilities.
     *
     * Called from both load paths, because the two must not be able to disagree about what mounting means.
     *
     * @param id     The host's id for this plugin; assigned to `entry` before the plugin runs.
     * @param entry  A mapped, judged plugin that is already in the mount table.
     * @param detail Receives the plugin's own sentence for a refusal, or the host's when it gave none.
     *
     * @ownership   observes
     * @thread      main
     * @pre         `entry.library.valid()` and no contribution of this plugin is recorded yet
     * @post        On failure the plugin's contributions have been withdrawn and `entry` is uninstalled
     * @invariant   On success every contribution is recorded and the provider is registered
     * @errors      The plugin's own code, `plugin_incompatible` for a refused negotiation
     * @complexity  O(1) plus the plugin's own registration
     * @nondet      Runs plugin code
     * @frozen      no
     * @tests       host.loads_a_real_plugin_and_mounts_what_it_registers,
     *              host.reports_a_plugins_own_reason_for_refusing
     */
    [[nodiscard]] diag::ErrorCode install_mounted(const std::string& id, Mounted& entry,
                                                  std::string& detail) noexcept;

    plugin::Capability grant_;
    graph::NodeTypeRegistry node_types_{};
    graph::kernels::KernelRegistry kernels_{};
    runtime::InstrumentRegistry instruments_{};
    runtime::FormatRegistry formats_{};
    authoring::Registry capabilities_{};
    /// The mount table. A `std::vector` because mount order is reported and the set is small; lookups are
    /// linear.
    std::vector<Mounted> mounted_{};
    /// What this build registers itself, under `kBuiltinOrigin`. Kept apart from `mounted_` because it is not a
    /// plugin: it has no manifest, no library and no capability declaration, and listing it among the plugins
    /// would make `mounted_ids()` answer a question nobody asked.
    Ledger builtins_{};
    /// The borrowed view `mounted_plugins()` returns, refreshed on every change to `mounted_`.
    std::vector<std::string_view> mounted_view_{};
    /// One provider per mounted plugin, kept alive for as long as the capability registry refers to it.
    ///
    /// Held by `unique_ptr` rather than by value, because `Registry::provider_of` hands out a raw pointer: a
    /// `std::vector<ManifestProvider>` would invalidate every pointer it had already given out the moment a
    /// second plugin mounted, and the caller would have no way to know.
    std::vector<std::unique_ptr<ManifestProvider>> providers_{};
};

}  // namespace qp::host
