/**
 * @file host.cpp
 * @brief Implementation of the composition root: mount, record, withdraw.
 */
#include <qp/host/host.hpp>

#include <qp/runtime/file/file.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

namespace qp::host {
namespace {

/// @brief Every capability bit this build knows, in declaration order.
///
/// Written out rather than derived from `kKnownCapabilities`, because "which bits exist" has to be a list
/// somebody can read. A loop over bit positions would report a bit as a capability the moment it was added to
/// the enum off the end of the known set, which is exactly the mistake the known-set check exists to catch.
[[nodiscard]] const std::vector<plugin::Capability>& all_capability_bits() {
    static const std::vector<plugin::Capability> bits{
        plugin::Capability::node_types,      plugin::Capability::port_types,
        plugin::Capability::kernels,         plugin::Capability::view_items,
        plugin::Capability::file_io,         plugin::Capability::field_domain,
        plugin::Capability::particle_domain, plugin::Capability::render_domain};
    return bits;
}

/// @brief The first bit in `declared` that `grant` does not cover, or `none` when every bit is covered.
[[nodiscard]] plugin::Capability first_ungranted(plugin::Capability declared,
                                                plugin::Capability grant) noexcept {
    for (const plugin::Capability bit : all_capability_bits()) {
        if (plugin::has_capability(declared, bit) && !plugin::has_capability(grant, bit)) return bit;
    }
    return plugin::Capability::none;
}

/// @brief The file's name, used as a stand-in identity when no manifest could be read.
[[nodiscard]] std::string file_name_of(const std::string& path) {
    // Through `runtime::to_path`, because the path arrived from the operating system as bytes and the naive
    // `std::filesystem::path{std::string}` would read them in the ANSI code page on Windows.
    return runtime::to_path(path).filename().string();
}

/// @brief A sentence for a loader failure, naming the plugin where the manifest was readable.
[[nodiscard]] std::string sentence_for(plugin::LoadFailure failure, const plugin::Manifest& manifest,
                                       bool has_manifest) {
    const bool named = has_manifest && !manifest.name.empty();
    const std::string who = named ? manifest.name : std::string{"it"};
    const std::string refused_as =
        named ? std::string{"its manifest was refused ("} +
                    std::string{plugin::to_string(plugin::judge(manifest, plugin::current_host()))} + ")"
              : std::string{"its manifest was refused"};

    switch (failure) {
        case plugin::LoadFailure::file_missing:
            return "no such file";
        case plugin::LoadFailure::library_unmappable:
            return "the operating system refused to map it: wrong architecture, a missing dependency, or a "
                   "corrupt file";
        case plugin::LoadFailure::entry_point_missing:
            return std::string{"it exports no "} + plugin::kEntryPointName +
                   ", so it is not a plugin library";
        case plugin::LoadFailure::plugin_declined:
            return who + " declined this host: it needs something no manifest field expresses";
        case plugin::LoadFailure::wrong_tag:
            return "its export block carries the wrong tag, so these bytes are not a plugin's exports";
        case plugin::LoadFailure::wrong_exports_version:
            return "its export block version is not one this build can read";
        case plugin::LoadFailure::manifest_missing:
            return "its export block carries no manifest";
        case plugin::LoadFailure::unregister_missing:
            return "it has no unregister callback, so it could never be unloaded safely";
        case plugin::LoadFailure::manifest_rejected:
            return who + ": " + refused_as;
        case plugin::LoadFailure::registration_failed:
            return who + " failed while installing its contributions";
        case plugin::LoadFailure::ok:
            return "loaded";
    }
    return "refused for an unnamed reason";
}

/// @brief A sentence for a capability refusal.
[[nodiscard]] std::string needs_refused_sentence(const plugin::Manifest& manifest,
                                                plugin::Capability bit) {
    const std::string who = manifest.name.empty() ? manifest.id : manifest.name;
    return who + " needs the '" + plugin::to_string(bit) +
           "' capability, which this host does not grant";
}

/// @brief The plural-aware contribution summary, or a statement that there were none.
[[nodiscard]] std::string contributions_sentence(std::size_t count) {
    if (count == 0) return "no contributions";
    return std::to_string(count) + (count == 1 ? " contribution" : " contributions");
}

}  // namespace

std::vector<authoring::CapabilityId> PluginHost::ManifestProvider::offers() const noexcept {
    std::vector<authoring::CapabilityId> out;
    for (const plugin::Capability bit : all_capability_bits()) {
        if (plugin::has_capability(declared_, bit)) {
            out.push_back(std::string{"qp.plugin."} + plugin::to_string(bit));
        }
    }
    return out;
}

std::string LoadReport::to_text() const {
    std::string out;
    for (const PluginOutcome& outcome : mounted_plugins) {
        out += "mounted " + outcome.id + " (" + outcome.path + ") with " +
               contributions_sentence(outcome.contributions) + "\n";
    }
    for (const PluginOutcome& outcome : refused) {
        out += "refused " + outcome.id + " (" + outcome.path + "): " + outcome.detail + "\n";
    }
    return out;
}

PluginHost::~PluginHost() {
    // Unloaded here rather than left to `mounted_`'s destructor, which would run after the registries were
    // already destroyed -- and every plugin's `unregister` is entitled to touch them.
    for (Mounted& entry : mounted_) entry.library.unload();
    mounted_.clear();
}

std::string_view PluginHost::mounting_plugin() const noexcept {
    return registering_ != nullptr ? std::string_view{registering_->id} : std::string_view{};
}

bool PluginHost::granted(plugin::Capability bit) const noexcept {
    return plugin::has_capability(grant_, bit);
}

void PluginHost::refresh_mounted_view() {
    mounted_view_.clear();
    mounted_view_.reserve(mounted_.size());
    for (const Mounted& entry : mounted_) mounted_view_.push_back(entry.id);
}

std::vector<std::string_view> PluginHost::mounted_plugins() const noexcept {
    return mounted_view_;
}

diag::ErrorCode PluginHost::add_node_type(graph::NodeDesc desc) noexcept {
    if (registering_ == nullptr) return diag::ErrorCode::plugin_load_failed;
    const std::string name = desc.type_name;
    const diag::Result<void> added = node_types_.register_type(std::move(desc));
    if (!added) return added.error();
    registering_->ledger.node_types.push_back(name);
    ++registering_->ledger.total;
    return diag::ErrorCode::ok;
}

diag::ErrorCode PluginHost::add_kernel(const graph::kernels::KernelDesc& desc) noexcept {
    if (registering_ == nullptr) return diag::ErrorCode::plugin_load_failed;
    const diag::Result<graph::kernels::KernelId> added = kernels_.add(desc);
    if (!added) return added.error();
    registering_->ledger.kernels.push_back(added.value());
    ++registering_->ledger.total;
    return diag::ErrorCode::ok;
}

diag::ErrorCode PluginHost::add_instrument(runtime::IInstrument* instrument) noexcept {
    if (registering_ == nullptr) return diag::ErrorCode::plugin_load_failed;
    // Read before the registration, because a failure below returns without touching the record and a success
    // needs the id. A null instrument's id is empty, which `add` refuses as `invalid_argument`.
    const std::string id =
        instrument != nullptr ? std::string{instrument->describe().id} : std::string{};
    const diag::Result<void> added = instruments_.add(instrument);
    if (!added) return added.error();
    registering_->ledger.instruments.push_back(id);
    ++registering_->ledger.total;
    return diag::ErrorCode::ok;
}

diag::ErrorCode PluginHost::add_exporter(runtime::IExporter* exporter) noexcept {
    if (registering_ == nullptr) return diag::ErrorCode::plugin_load_failed;
    const std::string name =
        exporter != nullptr ? std::string{exporter->format().name} : std::string{};
    const diag::Result<void> added = formats_.add(exporter);
    if (!added) return added.error();
    registering_->ledger.formats.push_back(name);
    ++registering_->ledger.total;
    return diag::ErrorCode::ok;
}

diag::ErrorCode PluginHost::refuse(const char* reason) noexcept {
    if (registering_ == nullptr) return diag::ErrorCode::plugin_load_failed;
    registering_->refusal = reason != nullptr ? std::string{reason} : std::string{};
    return diag::ErrorCode::plugin_load_failed;
}

std::string_view PluginHost::origin_of(std::string_view name) const noexcept {
    for (const Mounted& entry : mounted_) {
        for (const std::string& type_name : entry.ledger.node_types) {
            if (type_name == name) return std::string_view{entry.id};
        }
        // A device's id answers here too. The two are one record of "what did this plugin contribute" because
        // the question a caller asks is one question -- where did this name come from -- and a second lookup
        // would be a second list that could fall out of step with the first.
        for (const std::string& id : entry.ledger.instruments) {
            if (id == name) return std::string_view{entry.id};
        }
    }
    for (const std::string& type_name : builtins_.node_types) {
        if (type_name == name) return kBuiltinOrigin;
    }
    for (const std::string& id : builtins_.instruments) {
        if (id == name) return kBuiltinOrigin;
    }
    return std::string_view{};
}

diag::ErrorCode PluginHost::add_builtin_node_type(graph::NodeDesc desc) noexcept {
    // The same path a plugin's contribution takes, minus the plugin: the ledger is what makes the type
    // attributable and removable, and a built-in that took a different path would be the one kind of
    // contribution `origin_of` could not answer for.
    const std::string name = desc.type_name;
    const diag::Result<void> added = node_types_.register_type(std::move(desc));
    if (!added) return added.error();
    builtins_.node_types.push_back(name);
    ++builtins_.total;
    return diag::ErrorCode::ok;
}

void PluginHost::clear_builtin_node_types() noexcept {
    withdraw(builtins_);
    builtins_ = Ledger{};
}

diag::ErrorCode PluginHost::add_builtin_instrument(runtime::IInstrument* instrument) noexcept {
    // Read the id before registering, for the reason `add_instrument` gives: a failure returns without touching
    // the record, and a success needs the id to name what it added.
    const std::string id =
        instrument != nullptr ? std::string{instrument->describe().id} : std::string{};
    const diag::Result<void> added = instruments_.add(instrument);
    if (!added) return added.error();
    builtins_.instruments.push_back(id);
    ++builtins_.total;
    return diag::ErrorCode::ok;
}

void PluginHost::clear_builtin_instruments() noexcept {
    // Only the instruments, not the whole built-in ledger: clearing a device must not take a node type with it,
    // and a caller that wants both gone calls both. The first version of this method reset `builtins_`, which
    // silently removed every built-in node type as well -- and the window that called it to make room for a
    // second set of devices lost its palette.
    for (const std::string& id : builtins_.instruments) (void)instruments_.remove(id);
    builtins_.total -= builtins_.instruments.size();
    builtins_.instruments.clear();
}

bool PluginHost::is_mounted(std::string_view plugin_id) const noexcept {
    return std::any_of(mounted_.begin(), mounted_.end(),
                       [plugin_id](const Mounted& entry) { return entry.id == plugin_id; });
}

void PluginHost::forget(const std::string& plugin_id) noexcept {
    // The capability registry is keyed by plugin id, so unloading by id removes every offer it made -- there is
    // no second list of capability ids that could fall out of step with the first.
    (void)capabilities_.unload(plugin_id);
    const auto first = std::remove_if(providers_.begin(), providers_.end(),
                                      [&plugin_id](const std::unique_ptr<ManifestProvider>& p) {
                                          return p->plugin_id() == plugin_id;
                                      });
    providers_.erase(first, providers_.end());
}

void PluginHost::withdraw(const Ledger& ledger) noexcept {
    // Reverse order of contribution. The order does not matter to any one registry -- each removal names what
    // it removes -- but it does matter to the reader: the record shrinks the way it grew.
    for (const std::string& name : ledger.formats) (void)formats_.remove(name);
    for (const std::string& id : ledger.instruments) (void)instruments_.remove(id);
    for (const graph::kernels::KernelId id : ledger.kernels) (void)kernels_.drop(id);
    for (const std::string& name : ledger.node_types) (void)node_types_.remove_type(name);
}

void PluginHost::withdraw(Mounted& entry) noexcept {
    withdraw(entry.ledger);
    forget(entry.id);
    entry.ledger = Ledger{};
}

diag::ErrorCode PluginHost::install_mounted(const std::string& id, Mounted& entry,
                                            std::string& detail) noexcept {
    entry.id = id;
    registering_ = &entry;
    const diag::ErrorCode code = entry.library.install(this);
    const std::string refusal = entry.refusal;
    registering_ = nullptr;
    if (code != diag::ErrorCode::ok) {
        detail = !refusal.empty() ? refusal : std::string{"it failed while installing its contributions"};
        return code;
    }

    // Registered as a capability provider only once its contributions are live: a provider that became
    // reachable before its plugin finished installing would let a caller reach half a plugin.
    providers_.push_back(std::make_unique<ManifestProvider>(id, entry.library.manifest().capabilities, this));
    const authoring::NegotiationVerdict verdict = capabilities_.negotiate(*providers_.back(), grant_);
    if (verdict != authoring::NegotiationVerdict::granted) {
        providers_.pop_back();
        detail = std::string{"its capabilities were refused during negotiation ("} +
                 authoring::to_string(verdict) + ")";
        return diag::ErrorCode::plugin_incompatible;
    }
    return diag::ErrorCode::ok;
}

PluginOutcome PluginHost::load(const std::string& path) {
    PluginOutcome outcome;
    outcome.path = path;

    Mounted entry;
    const plugin::LoadOutcome loaded = plugin::load_plugin(path, nullptr, &entry.library);
    outcome.id = loaded.has_manifest && !loaded.manifest.id.empty() ? loaded.manifest.id
                                                                  : file_name_of(path);
    outcome.name = loaded.has_manifest && !loaded.manifest.name.empty() ? loaded.manifest.name
                                                                      : outcome.id;
    if (!loaded.ok()) {
        outcome.code = diag::ErrorCode::plugin_load_failed;
        outcome.detail = sentence_for(loaded.failure, loaded.manifest, loaded.has_manifest);
        return outcome;
    }

    if (is_mounted(outcome.id)) {
        entry.library.unload();
        outcome.code = diag::ErrorCode::plugin_incompatible;
        outcome.detail = outcome.name + " is already mounted";
        return outcome;
    }

    if (const plugin::Capability missing = first_ungranted(loaded.manifest.capabilities, grant_);
        missing != plugin::Capability::none) {
        entry.library.unload();
        outcome.code = diag::ErrorCode::plugin_capability_missing;
        outcome.detail = needs_refused_sentence(loaded.manifest, missing);
        return outcome;
    }

    mounted_.push_back(std::move(entry));
    // Refreshed before `install`, so a plugin that checks `mounted_plugins()` for its dependency reads the
    // table as it stands now rather than as it stood before the previous plugin was appended.
    refresh_mounted_view();
    Mounted& slot = mounted_.back();
    std::string why;
    const diag::ErrorCode code = install_mounted(outcome.id, slot, why);
    if (code != diag::ErrorCode::ok) {
        withdraw(slot);
        slot.library.unload();
        mounted_.pop_back();
        refresh_mounted_view();
        outcome.code = code;
        outcome.detail = outcome.name + ": " + why;
        return outcome;
    }

    outcome.mounted = true;
    outcome.contributions = slot.ledger.total;
    outcome.detail = outcome.name + " mounted with " + contributions_sentence(outcome.contributions);
    return outcome;
}

LoadReport PluginHost::load_directory(const std::string& directory, std::string_view extension) {
    LoadReport report;

    std::error_code ec;
    const std::filesystem::path root = runtime::to_path(directory);
    if (!std::filesystem::is_directory(root, ec)) {
        // Not an error. A build with no content directory is a legitimate build, and `Run` already explains
        // what a graph with no operator means.
        return report;
    }

    // Sorted by name, because a directory listing is not ordered and two machines would otherwise mount the
    // same plugins in different orders -- which shows up as a different type winning a duplicate, or a
    // different palette order, on two machines looking at one document.
    const std::string wanted = extension.empty() ? std::string{} : "." + std::string{extension};
    std::vector<std::string> candidates;
    for (const std::filesystem::directory_entry& item : std::filesystem::directory_iterator(root, ec)) {
        if (!item.is_regular_file(ec)) continue;
        if (!wanted.empty() && item.path().extension() != std::filesystem::path{wanted}) continue;
        candidates.push_back(item.path().string());
    }
    std::sort(candidates.begin(), candidates.end());

    // Phase one: map and judge each candidate, install nothing.
    //
    // Installation is deferred because load order comes from the manifests, and the manifests only exist once
    // every library is mapped. Installing one file at a time would install a dependent before its dependency
    // whenever the file order happened to be wrong, and the plugin would then fail for a reason that has
    // nothing to do with the plugin.
    std::vector<Mounted> staged;
    staged.reserve(candidates.size());
    for (const std::string& path : candidates) {
        Mounted entry;
        const plugin::LoadOutcome loaded = plugin::load_plugin(path, nullptr, &entry.library);
        PluginOutcome outcome;
        outcome.path = path;
        outcome.id = loaded.has_manifest && !loaded.manifest.id.empty() ? loaded.manifest.id
                                                                      : file_name_of(path);
        outcome.name = loaded.has_manifest && !loaded.manifest.name.empty() ? loaded.manifest.name
                                                                          : outcome.id;

        bool installable = loaded.ok();
        if (!installable) {
            outcome.code = diag::ErrorCode::plugin_load_failed;
            outcome.detail = sentence_for(loaded.failure, loaded.manifest, loaded.has_manifest);
        } else if (const plugin::Capability missing =
                       first_ungranted(loaded.manifest.capabilities, grant_);
                   missing != plugin::Capability::none) {
            installable = false;
            entry.library.unload();
            outcome.code = diag::ErrorCode::plugin_capability_missing;
            outcome.detail = needs_refused_sentence(loaded.manifest, missing);
        } else if (is_mounted(outcome.id)) {
            installable = false;
            entry.library.unload();
            outcome.code = diag::ErrorCode::plugin_incompatible;
            outcome.detail = outcome.name + " is already mounted";
        }

        if (!installable) {
            report.refused.push_back(std::move(outcome));
            continue;
        }

        entry.id = outcome.id;
        staged.push_back(std::move(entry));
        report.mounted_plugins.push_back(std::move(outcome));
    }

    /// @brief The slots whose contributions are recorded, in install order. Filled during phase two.
    std::vector<std::size_t> installed_slots;

    /// @brief Refuses the whole batch, leaving every registry as it was found.
    ///
    /// The plugins that already installed are withdrawn through the mount table -- they were moved there as
    /// they installed, which is what lets a later plugin see its dependency -- and the rest are released
    /// without ever having been entered. Withdrawing a plugin that never installed would be harmless, since its
    /// record is empty; releasing its library before its `register_into` ran is not something to leave to
    /// chance, so the two lists stay separate.
    const auto refuse_all = [this, &report, &staged,
                             &installed_slots](diag::ErrorCode code, const std::string& detail) {
        const std::size_t installed = installed_slots.size();
        // The mount table holds the installed plugins appended in dependency order, so unloading the last
        // `installed` of it, newest first, is both the withdrawal and the library release.
        for (std::size_t n = installed; n > 0; --n) {
            (void)this->unload(mounted_.back().id);
        }
        // Whatever is left in `staged` was never entered, so its mapping is released directly.
        for (Mounted& entry : staged) entry.library.unload();
        // Every contribution is gone now, so the report must say so. Reporting a count for a plugin whose
        // types were just removed would be the report claiming something the registries contradict.
        for (PluginOutcome& outcome : report.mounted_plugins) {
            outcome.mounted = false;
            outcome.contributions = 0;
            outcome.code = code;
            outcome.detail = outcome.name + ": " + detail;
            report.refused.push_back(std::move(outcome));
        }
        report.mounted_plugins.clear();
        installed_slots.clear();
    };

    if (!report.refused.empty()) {
        // A candidate that was refused is a refusal of the **set**, because a partially loaded plugin set is the
        // failure this platform exists to prevent elsewhere in a different costume: with a dependency missing, a
        // graph built against it produces numbers that are wrong rather than absent, and the user watches a
        // simulation run to completion.
        //
        // Mapped candidates are released and reported too, rather than dropped. A report that mentioned only the
        // file that failed would leave the user unsure whether the other two mounted.
        const std::string reason =
            "the set was refused because " + std::to_string(report.refused.size()) +
            (report.refused.size() == 1 ? " other candidate failed" : " other candidates failed");
        while (!staged.empty()) {
            Mounted& entry = staged.back();
            entry.library.unload();
            staged.pop_back();
        }
        for (PluginOutcome& outcome : report.mounted_plugins) {
            outcome.mounted = false;
            outcome.contributions = 0;
            outcome.code = diag::ErrorCode::plugin_incompatible;
            outcome.detail = outcome.name + ": " + reason;
            report.refused.push_back(std::move(outcome));
        }
        report.mounted_plugins.clear();
        return report;
    }

    // Dependency order, recomputed from the manifests that actually loaded. Ties break by input index, so the
    // same set of files produces one order rather than "an order".
    //
    // Dependencies outside the set are dropped from these copies, and that is not a convenience. A plugin may
    // legitimately depend on something the **host** built in statically -- `plugins/mechanics` depends on
    // nothing, but a content plugin shipped beside one that is compiled in is an ordinary arrangement. Asking
    // `load_order` to sort a set by a dependency it cannot see reports a **cycle**, which names the wrong
    // problem and sends the user to their file names.
    std::vector<plugin::Manifest> manifests;
    manifests.reserve(staged.size());
    for (const Mounted& entry : staged) {
        plugin::Manifest copy = entry.library.manifest();
        const auto keep = std::remove_if(copy.dependencies.begin(), copy.dependencies.end(),
                                         [&staged](const std::string& id) {
                                             return std::none_of(
                                                 staged.begin(), staged.end(),
                                                 [&id](const Mounted& other) { return other.id == id; });
                                         });
        copy.dependencies.erase(keep, copy.dependencies.end());
        manifests.push_back(std::move(copy));
    }

    const std::vector<std::size_t> order = plugin::load_order(manifests);
    if (order.size() != manifests.size()) {
        refuse_all(diag::ErrorCode::plugin_incompatible,
                   "it is part of a dependency cycle, so no load order exists");
        return report;
    }

    // Phase two: install in dependency order. The first failure refuses the whole set -- see the
    // all-or-nothing argument in the header -- and says which plugin caused it.
    //
    // Each plugin enters the mount table as it installs rather than at the end, because `mounted_plugins()` is
    // how a plugin checks that its dependency is really there, and a table filled in afterwards would tell
    // every plugin that nothing was.
    for (const std::size_t slot : order) {
        Mounted& entry = staged[slot];
        const std::string id = entry.id;
        std::string why;
        const diag::ErrorCode code = install_mounted(id, entry, why);
        if (code != diag::ErrorCode::ok) {
            refuse_all(code, why);
            return report;
        }
        installed_slots.push_back(slot);
        // Reported before the move, and then the entry itself moves into the mount table: reading its ledger
        // afterwards would be reading a moved-from object.
        for (PluginOutcome& outcome : report.mounted_plugins) {
            if (outcome.id != id) continue;
            outcome.mounted = true;
            outcome.contributions = entry.ledger.total;
            outcome.detail = outcome.name + " mounted with " +
                             contributions_sentence(outcome.contributions);
        }
        mounted_.push_back(std::move(entry));
        refresh_mounted_view();
    }

    return report;
}

diag::Result<void> PluginHost::unload(std::string_view plugin_id) {
    for (std::size_t i = 0; i < mounted_.size(); ++i) {
        if (mounted_[i].id != plugin_id) continue;
        Mounted& entry = mounted_[i];
        // The record first, then the plugin's own callback inside `unload()`, then the mapping. A plugin that
        // forgot to remove one of its own contributions can no longer leave a dangling descriptor behind.
        withdraw(entry);
        entry.library.unload();
        mounted_.erase(mounted_.begin() + static_cast<std::ptrdiff_t>(i));
        refresh_mounted_view();
        return {};
    }
    return diag::ErrorCode::unknown_node;
}

std::vector<std::string> PluginHost::mounted_ids() const {
    std::vector<std::string> out;
    out.reserve(mounted_.size());
    for (const Mounted& entry : mounted_) out.push_back(entry.id);
    return out;
}

}  // namespace qp::host
