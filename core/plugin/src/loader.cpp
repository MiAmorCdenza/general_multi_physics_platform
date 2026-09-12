/**
 * @file loader.cpp
 * @brief Maps plugin libraries and installs their contributions.
 *
 * The platform split lives here and nowhere else: `LoadLibraryExW` on Windows,
 * `dlopen` elsewhere. It is confined to this translation unit so that the header
 * -- and every plugin author reading it -- needs no system header, and so the
 * difference between the two platforms is two small functions rather than an
 * `#ifdef` running through the module.
 *
 * Nothing above this file knows whether unloading is even possible. That matters
 * because it is **not** guaranteed everywhere: POSIX `dlclose` may leave a library
 * mapped when something still references it, and a host that assumed otherwise
 * would have registries pointing into memory it believes it released. So the
 * unload path is ordered to be correct regardless -- registrations first, then the
 * mapping, best-effort -- and no caller depends on the mapping actually going away.
 */
#include <qp/plugin/loader.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#if defined(_WIN32)
// Guarded rather than defined outright: MinGW's libstdc++ already defines
// NOMINMAX in its own os_defines.h, so an unguarded define is a redefinition
// warning on every GCC build of this file -- noise that trains readers to ignore
// warnings in the one translation unit that talks to the OS.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace qp::plugin {
namespace {

/// @brief Whether a regular file exists at this path, asked without mapping it.
///
/// Exists so the two causes of "the OS would not give me a library" can be told
/// apart. They are a packaging mistake and a build mistake, they send the reader to
/// different places, and collapsing them into one message costs more than this call.
bool file_exists(const std::string& path) noexcept {
#if defined(_WIN32)
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (needed <= 0) return false;
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), needed) <= 0) return false;
    const DWORD attrs = ::GetFileAttributesW(wide.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    // No <filesystem> here on purpose: it drags in an allocator-heavy header to
    // answer one question, and this translation unit is deliberately close to the OS.
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
#endif
}

/// @brief Opens a shared library, or returns null.
///
/// `LOAD_WITH_ALTERED_SEARCH_PATH` rather than a plain load: a plugin directory
/// typically ships its own dependencies beside the plugin, and the default search
/// order looks in the **host executable's** directory first. A different build of a
/// shared dependency sitting next to the host would then silently win over the one
/// shipped with the plugin, and the plugin would run against libraries it was never
/// built with -- a failure that appears as wrong physics, not as a load error.
void* open_library(const std::string& path) noexcept {
#if defined(_WIN32)
    // Wide, not ANSI: the path came from a user's directory listing, and on a
    // Chinese Windows most such paths are outside the ANSI code page.
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (needed <= 0) return nullptr;
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), needed) <= 0) return nullptr;
    return static_cast<void*>(::LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
#else
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

/// @brief Releases a mapped library. Best-effort by construction; see the file comment.
void close_library(void* handle) noexcept {
    if (handle == nullptr) return;
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

/// @brief Looks up the one symbol both sides agree on.
void* find_symbol(void* handle, const char* name) noexcept {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return ::dlsym(handle, name);
#endif
}

/// @brief Reads the export block and checks it is really one, in a fixed order.
///
/// The ordering is the point. `tag` and `exports_version` are plain integers at
/// known offsets, so they can be read from any block of bytes without
/// misinterpretation. Only once both match is it safe to touch `manifest` and the
/// callbacks, which are **pointers**: reading a pointer out of a block that is not a
/// `PluginExports`, then following it, is precisely the failure this check exists to
/// prevent.
LoadFailure inspect_exports(const PluginExports* exports) noexcept {
    if (exports == nullptr) return LoadFailure::plugin_declined;
    if (exports->tag != kExportsTag) return LoadFailure::wrong_tag;
    if (exports->exports_version != kExportsVersion) return LoadFailure::wrong_exports_version;
    if (exports->manifest == nullptr) return LoadFailure::manifest_missing;
    if (exports->unregister == nullptr) return LoadFailure::unregister_missing;
    return LoadFailure::ok;
}

}  // namespace

Manifest to_manifest(const PluginManifestC& c) {
    // One null-checking helper rather than repeated ternaries: the check is the part
    // that matters, and repeating it inline is how one of them ends up missing.
    const auto text = [](const char* s) -> std::string {
        return s != nullptr ? std::string{s} : std::string{};
    };

    Manifest m;
    m.id = text(c.id);
    m.name = text(c.name);
    m.version = abi::Version{c.version_major, c.version_minor, c.version_patch};
    m.layout = c.layout;
    m.capabilities = static_cast<Capability>(c.capabilities);
    if (c.dependencies != nullptr) {
        m.dependencies.reserve(c.dependency_count);
        for (std::uint32_t i = 0; i < c.dependency_count; ++i) {
            // `c.dependencies[i]` may itself be null in a malformed block. Treating it
            // as an empty id rather than dereferencing keeps this function total: an
            // empty id names nothing, so judge_with_dependencies reports it missing,
            // which is a better answer than a fault inside the host.
            m.dependencies.push_back(text(c.dependencies[i]));
        }
    }
    return m;
}

const char* to_string(LoadFailure f) noexcept {
    switch (f) {
        case LoadFailure::ok: return "ok";
        case LoadFailure::file_missing: return "file_missing";
        case LoadFailure::library_unmappable: return "library_unmappable";
        case LoadFailure::entry_point_missing: return "entry_point_missing";
        case LoadFailure::plugin_declined: return "plugin_declined";
        case LoadFailure::wrong_tag: return "wrong_tag";
        case LoadFailure::wrong_exports_version: return "wrong_exports_version";
        case LoadFailure::manifest_missing: return "manifest_missing";
        case LoadFailure::unregister_missing: return "unregister_missing";
        case LoadFailure::manifest_rejected: return "manifest_rejected";
        case LoadFailure::registration_failed: return "registration_failed";
    }
    return "unknown";
}

LoadedPlugin::~LoadedPlugin() { unload(); }

LoadedPlugin::LoadedPlugin(LoadedPlugin&& other) noexcept
    : handle_(other.handle_),
      exports_(other.exports_),
      manifest_(std::move(other.manifest_)),
      host_context_(other.host_context_),
      registered_(other.registered_),
      path_(std::move(other.path_)) {
    other.handle_ = nullptr;
    other.exports_ = nullptr;
    other.registered_ = false;
}

LoadedPlugin& LoadedPlugin::operator=(LoadedPlugin&& other) noexcept {
    if (this == &other) return *this;
    // Unload first. Assigning over a live plugin would otherwise drop the only handle
    // to a still-mapped library, leaving its registrations in place with no way to
    // ever take them down. A leaked mapping is the mild outcome; a registry
    // permanently holding non-owning pointers to code nobody can unregister is the
    // severe one.
    unload();
    handle_ = other.handle_;
    exports_ = other.exports_;
    manifest_ = std::move(other.manifest_);
    host_context_ = other.host_context_;
    registered_ = other.registered_;
    path_ = std::move(other.path_);
    other.handle_ = nullptr;
    other.exports_ = nullptr;
    other.registered_ = false;
    return *this;
}

bool LoadedPlugin::valid() const noexcept { return handle_ != nullptr; }

const Manifest& LoadedPlugin::manifest() const noexcept { return manifest_; }

const std::string& LoadedPlugin::path() const noexcept { return path_; }

bool LoadedPlugin::contributes() const noexcept { return registered_; }

diag::ErrorCode LoadedPlugin::install(void* host_context) noexcept {
    if (handle_ == nullptr || exports_ == nullptr) return diag::ErrorCode::plugin_load_failed;
    if (registered_) return diag::ErrorCode::ok;
    host_context_ = host_context;
    if (exports_->register_into == nullptr) {
        // Nothing to install is a valid plugin: one that exists only to be depended
        // on, or whose contribution is a side effect of being mapped.
        registered_ = true;
        return diag::ErrorCode::ok;
    }
    const diag::ErrorCode code = exports_->register_into(host_context);
    if (code != diag::ErrorCode::ok) {
        // The plugin is half-installed by its own admission. Undo it here, while the
        // library is still mapped and `unregister` is still callable, rather than
        // leaving the mess for whoever notices later. `unregister` is required to be
        // idempotent and safe on partial state precisely for this path.
        exports_->unregister(host_context);
        return code;
    }
    registered_ = true;
    return diag::ErrorCode::ok;
}

void LoadedPlugin::unload() noexcept {
    if (handle_ == nullptr) return;
    // Unregister first, never the other way round. After `close_library` returns,
    // `exports_` points into unmapped memory, so calling `unregister` afterwards is a
    // jump into a hole. This ordering is the entire safety argument for letting
    // registries hold non-owning pointers to plugin-owned objects.
    //
    // Called whenever `register_into` was entered, not only on success: a partial
    // registration is the case most in need of cleanup, and `install` returning an
    // error still leaves the plugin's earlier contributions in place.
    if (registered_ && exports_ != nullptr && exports_->unregister != nullptr) {
        exports_->unregister(host_context_);
    }
    close_library(handle_);
    handle_ = nullptr;
    exports_ = nullptr;
    registered_ = false;
    host_context_ = nullptr;
    path_.clear();
    manifest_ = Manifest{};
}

bool LoadReport::all_ok() const noexcept {
    return std::all_of(outcomes.begin(), outcomes.end(),
                       [](const LoadOutcome& o) { return o.ok(); });
}

const LoadOutcome* LoadReport::first_failure() const noexcept {
    for (const LoadOutcome& o : outcomes) {
        if (!o.ok()) return &o;
    }
    return nullptr;
}

LoadOutcome load_plugin(const std::string& path, void* /*host_context*/, LoadedPlugin* out) {
    LoadOutcome result;
    result.path = path;
    if (out == nullptr) {
        result.failure = LoadFailure::file_missing;
        return result;
    }

    if (!file_exists(path)) {
        result.failure = LoadFailure::file_missing;
        return result;
    }

    void* handle = open_library(path);
    if (handle == nullptr) {
        result.failure = LoadFailure::library_unmappable;
        return result;
    }

    // `reinterpret_cast` from a void* the OS filled in. The cast is only sound
    // because the symbol name is fixed by this header and the plugin is required to
    // export a function of exactly this type. Nothing here can verify that, which is
    // exactly why the exports block carries a tag and a version instead of trusting
    // the signature.
    auto* entry = reinterpret_cast<EntryPointFn>(find_symbol(handle, kEntryPointName));
    if (entry == nullptr) {
        close_library(handle);
        result.failure = LoadFailure::entry_point_missing;
        return result;
    }

    const PluginExports* exports = entry();
    const LoadFailure shape = inspect_exports(exports);
    if (shape != LoadFailure::ok) {
        close_library(handle);
        result.failure = shape;
        return result;
    }

    // Converted here, on the host's side, with the host's allocator. The descriptor
    // itself is never stored: it points into the library's read-only data, which is
    // valid for as long as the mapping is, but a `Manifest` that outlives the plugin
    // would otherwise hold borrowed pointers.
    result.manifest = to_manifest(*exports->manifest);
    result.has_manifest = true;

    // Judged with no dependency information: presence of a dependency is a property
    // of the *set*, and `load_plugins` re-judges with the real list once every
    // manifest is known. What is decided here is everything about this file alone.
    //
    // Before `register_into`, always. The alternative -- install first, check after --
    // means an incompatible plugin has already written into the registries by the
    // time anyone notices, and undoing that is a best-effort operation against a
    // plugin that is by definition not behaving as this host expects.
    if (judge(result.manifest, current_host()) != ManifestVerdict::ok) {
        close_library(handle);
        result.failure = LoadFailure::manifest_rejected;
        return result;
    }

    // Mapped and judged, but deliberately **not installed**: installation order is
    // derived from the manifests, and the manifests only exist once every library is
    // mapped. `install` is called by load_plugins.
    //
    // The manifest is **copied**, not moved, into the plugin. Moving it would leave
    // the outcome reporting an empty manifest while `has_manifest` still said true --
    // and the outcome's copy is the one a failure report reads, which is exactly the
    // case where the identity matters most.
    out->handle_ = handle;
    out->exports_ = exports;
    out->manifest_ = result.manifest;
    out->path_ = path;
    result.failure = LoadFailure::ok;
    return result;
}

LoadReport load_plugins(const std::vector<std::string>& paths, void* host_context) {
    LoadReport report;
    report.requested = paths.size();

    // Phase one: map everything, judge each manifest, install nothing.
    //
    // Installation is deferred because load order comes from the manifests. The
    // obvious alternative -- map and install one file at a time -- installs a
    // dependent before its dependency whenever the caller's file order happens to be
    // wrong, and the plugin then fails for a reason that has nothing to do with the
    // plugin. Directory order is not dependency order, and on Windows it is not even
    // alphabetical.
    struct Staged final {
        LoadedPlugin plugin{};
        LoadOutcome outcome{};
    };

    std::vector<Staged> staged;
    staged.reserve(paths.size());

    for (const std::string& path : paths) {
        Staged entry;
        LoadedPlugin plugin;
        entry.outcome = load_plugin(path, host_context, &plugin);
        entry.plugin = std::move(plugin);
        staged.push_back(std::move(entry));
    }

    // Dependency order, recomputed from the manifests that actually loaded. Only
    // plugins that judged ok take part: an unloadable plugin's manifest is not
    // evidence about anyone else's dependencies.
    std::vector<Manifest> manifests;
    std::vector<std::size_t> slot_of;  // index into `staged`
    for (std::size_t i = 0; i < staged.size(); ++i) {
        if (staged[i].outcome.ok()) {
            manifests.push_back(staged[i].plugin.manifest());
            slot_of.push_back(i);
        }
    }

    const std::vector<std::size_t> order = load_order(manifests);
    const bool cyclic = order.size() != manifests.size();

    if (cyclic) {
        // A cycle is a failure of the **set**, not of any single plugin, so it is
        // reported against every member rather than dropped silently. Dropping the
        // cyclic group would leave a session that starts and is quietly missing
        // plugins -- the user sees a working program and a hole in it.
        for (Staged& s : staged) {
            if (s.outcome.ok()) s.outcome.failure = LoadFailure::manifest_rejected;
            s.plugin.unload();
        }
        for (Staged& s : staged) report.outcomes.push_back(std::move(s.outcome));
        return report;
    }

    // Phase two: install in dependency order, stopping at the first failure.
    //
    // Stopping rather than continuing is deliberate. A failed install means the
    // registries are in a state this host does not understand; installing more
    // plugins into that state produces a session whose contents depend on how far the
    // loop got, which is worse than no session.
    for (const std::size_t slot : order) {
        Staged& s = staged[slot_of[slot]];
        const diag::ErrorCode code = s.plugin.install(host_context);
        if (code != diag::ErrorCode::ok) {
            s.outcome.failure = LoadFailure::registration_failed;
            break;
        }
    }

    for (Staged& s : staged) {
        report.outcomes.push_back(std::move(s.outcome));
    }

    const bool any_failed = std::any_of(report.outcomes.begin(), report.outcomes.end(),
                                        [](const LoadOutcome& o) { return !o.ok(); });
    if (any_failed) {
        // Refuse the whole set. Every mapped library is released, and each release
        // unregisters first, so the host is left exactly as it was found. A partial
        // plugin set is the failure this platform exists to prevent elsewhere in a
        // different costume: with a dependency missing, a graph built against it
        // produces numbers that are *wrong* rather than absent, and the user watches a
        // simulation run to completion.
        for (Staged& s : staged) s.plugin.unload();
        return report;
    }

    for (const std::size_t slot : order) {
        report.plugins.push_back(std::move(staged[slot_of[slot]].plugin));
    }
    return report;
}

}  // namespace qp::plugin
