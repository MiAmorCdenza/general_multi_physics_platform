/**
 * @file main.cpp
 * @brief The shell executable: builds the content host, then opens the editor.
 *
 * A thin `main`, and deliberately so. Everything with behaviour lives in `qp::views` or `qp::host` so that it
 * can be constructed and inspected without an event loop; an application whose logic exists only inside `main`
 * cannot be tested at all, and the first casualty is always the thing that was hard to reach.
 *
 * ## What this function actually decides
 *
 * Three things, in this order, and the order is the whole design:
 *
 *   1. **What content this build has.** Two sources, and they are different in kind. The statically linked
 *      plugins (`QP_HAS_*`) are this build's own content; the plugin **directory** is content somebody else
 *      built, loaded at startup through `qp::host::PluginHost`. Both end up in the same registries, which is
 *      what makes "everything is a plugin" a property of the platform rather than of a build.
 *   2. **How much to grant.** The host is constructed with an explicit capability grant rather than with
 *      "everything this build knows". A plugin whose manifest declares a bit outside the grant is refused
 *      before its code runs, and a build that granted everything could not express that decision at all.
 *   3. **What to say when it goes wrong.** Plugin loading is reported, never fatal: a directory with one
 *      broken library refuses the whole directory (see `PluginHost::load_directory`), and the window still
 *      opens, because a lab machine with a mistyped plugin path must still be able to open a saved document
 *      and read its numbers.
 *
 * A fourth decision was added later and belongs to the same list: **what can measure.** The rack of devices goes
 * into the host's registry before the window exists, because the measurement panel reads that registry while it
 * is being built -- and a device registered afterwards would be missing from the list until something refreshed
 * it, which nothing does. It is also the only way a build with no plugins loaded has any instrument at all, and
 * without one the platform's central loop is unreachable rather than merely quiet.
 *
 * @ownership   owns (the plugin host, the registered formats and the window)
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   No application state is created outside the window and the host
 * @errors      returns the Qt event loop's exit code
 * @frozen      no
 * @tests       host.loads_a_real_plugin_and_mounts_what_it_registers
 */
#include "editor_window.hpp"

#include <qp/host/host.hpp>
#include <qp/views/model/document_controller.hpp>
#include <qp/views/model/execution_binders.hpp>
#include <qp/views/model/run_providers.hpp>
#include <qp/views/model/export_controller.hpp>

#if defined(QP_HAS_MECHANICS_PLUGIN)
#include <qp/plugins/mechanics/mechanics_binder.hpp>
#endif

#if defined(QP_HAS_FORMAT_PLUGINS)
#include <qp/plugins/csv/csv_exporter.hpp>
#include <qp/plugins/qpjson/qpjson_format.hpp>
#endif

#if defined(QP_HAS_INSTRUMENTS_PLUGIN)
#include <qp/plugins/instruments/instruments.hpp>
#endif

#if defined(QP_HAS_EXPERIMENT_PLUGIN)
#include <qp/plugins/experiments/experiments.hpp>
#endif

#if defined(QP_HAS_MAGNETOSPHERE_PLUGIN)
#include <qp/plugins/magnetosphere/emitter.hpp>
#include <qp/plugins/magnetosphere/field_nodes.hpp>
#include <qp/plugins/magnetosphere/plan.hpp>
#include <qp/plugins/magnetosphere/run.hpp>
#include <qp/plugins/magnetosphere/field_lines_item.hpp>
#include <qp/plugins/magnetosphere/render_nodes.hpp>
#include <qp/plugins/magnetosphere/view_item.hpp>
#endif

#include <QApplication>
#include <QDebug>
#include <QtGlobal>

#include <filesystem>
#include <string>

namespace {

/**
 * @brief Where this build looks for content libraries that were not linked into it.
 *
 * `QP_PLUGIN_DIR` when it is set, because that is what a lab machine needs: the same build with a different
 * content directory, without a rebuild. Otherwise a `plugins` directory beside the executable, which is how a
 * packaged build ships its content. The environment variable wins rather than being a fallback because the
 * awkward case -- a developer testing one plugin against the shipped set -- is the one that has to be
 * expressible without moving files around.
 *
 * @ownership   owns the returned string
 * @thread      ui
 * @pre         none
 * @post        Returns a path even when no such directory exists; `load_directory` treats that as empty
 * @invariant   Never throws; allocation failure terminates, as elsewhere in this project
 * @errors      noexcept
 * @complexity  O(path length)
 * @nondet      Reads the environment
 * @frozen      no
 * @tests       host.scanning_a_directory_mounts_what_it_can_and_reports_the_rest
 */
[[nodiscard]] std::string plugin_directory(const char* argv0) {
    // `qEnvironmentVariable` rather than `std::getenv`, and not for convenience: the C runtime's `getenv` is
    // flagged as unsafe by the MSVC CRT (C4996) and a warning-as-error build then refuses the shell. Qt's
    // accessor is the same read on every platform this project builds for, and this file is Qt code.
    const QString from_env = qEnvironmentVariable("QP_PLUGIN_DIR");
    if (!from_env.isEmpty()) return from_env.toStdString();
    std::error_code ec;
    // `weakly_canonical` rather than `canonical`: the executable path is what the operating system reported,
    // and a build directory that has since been renamed should land in an empty scan rather than in a
    // diagnostic about the program's own location.
    const std::filesystem::path exe = std::filesystem::weakly_canonical(argv0 != nullptr ? argv0 : ".", ec);
    const std::filesystem::path beside = (ec ? std::filesystem::path{argv0 != nullptr ? argv0 : "."}
                                             : exe.parent_path()) /
                                         "plugins";
    return beside.string();
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // The composition root, built **first**, and the order is load-bearing rather than incidental.
    //
    // It used to be constructed after the formats, which was harmless while every format was self-contained --
    // and stopped being harmless the moment one of them needed a node catalog. A teacher's experiment package
    // describes each input it offers by asking the catalog what that input *is*: its label, its unit, its
    // bounds. So the catalog has to exist before the format does, and the plugins loaded from the directory
    // below are the ones that fill it. A format built against an empty catalog would not fail; it would
    // silently describe every parameter as a bare number.
    //
    // The grant is written out rather than taken from `kKnownCapabilities`, and that is a decision rather than
    // verbosity. A host that grants everything it knows cannot refuse anything, so the manifest check would
    // pass for every plugin that parses -- and "what is this session allowed to do" would be answered by
    // whichever plugins happened to be present. `field_domain` and `render_domain` are absent because no
    // content in this build contributes to them; adding a plugin that does is how they get added here.
    qp::host::PluginHost content_host{qp::plugin::Capability::node_types |
                                      qp::plugin::Capability::kernels |
                                      qp::plugin::Capability::file_io};
    const std::string content_dir = plugin_directory(argc > 0 ? argv[0] : nullptr);
    const qp::host::LoadReport loaded = content_host.load_directory(content_dir, "dll");
    if (loaded.attempts() > 0) {
        // To stderr, one line per attempt, because a plugin that did not mount is the single most common
        // reason a user's palette is missing an entry -- and a window that opened without saying so would
        // leave them reading the wrong file.
        qWarning().noquote() << QString::fromStdString(loaded.to_text()).trimmed();
    }

    // The one place that knows which plugins exist **in this build**. Mounting happens before the window is
    // built, because the window's controllers capture their lists during construction.
    //
    // Guarded by `QP_BUILD_PLUGINS`: with plugins off there is nothing to mount, and the Run action then
    // reports that no node has an operator -- which is the accurate description of that build.
#if defined(QP_HAS_MECHANICS_PLUGIN)
    qp::plugins::mechanics::MechanicsBinder mechanics;
    qp::views::model::mount_execution_binder(&mechanics);
#endif

    // The formats. Both live here rather than in a header, so they outlive the window and its controllers;
    // a format mounted from a temporary would leave the window holding a dangling pointer the first time a
    // user pressed Save.
#if defined(QP_HAS_FORMAT_PLUGINS)
    qp::plugins::qpjson::QpJsonFormat document_format;
    qp::views::model::mount_document_format(&document_format);

    qp::plugins::csv::CsvExporter trace_exporter;
    // Registered, not asserted: a duplicate or an unnamed format is refused by the registry, which is where
    // that rule belongs. Here the only answer available is to carry on -- one format failing to register
    // must not stop the window from opening.
    (void)qp::views::model::mount_export_format(&trace_exporter);
#endif

#if defined(QP_HAS_EXPERIMENT_PLUGIN) && defined(QP_HAS_FORMAT_PLUGINS)
    // The teaching-experiment bundle, mounted **over** the document format rather than beside it: it carries a
    // whole `.qpd` as a nested member, so the two share one graph parser and a teacher's file loads through the
    // same reader a `.qpd` does. Mounted second so the plain document is the default in a "save as" list -- a
    // teacher who wants an experiment picks it deliberately, and a user who never heard of one gets the
    // document.
    //
    // This is also the only place the composition happens, and it has to be here: `plugins/experiments` takes an
    // `IDocumentFormat&` rather than naming `qpjson`, so nothing but the application decides which format an
    // experiment is written against. A second document format -- a binary one, say -- becomes a second
    // experiment format by changing one line in this function and nothing in either plugin.
    qp::plugins::experiments::ExperimentFormat experiment_format{document_format,
                                                               content_host.node_types()};
    qp::views::model::mount_document_format(&experiment_format);
#endif

    // The measuring devices. They go through `add_builtin_instrument` for the same reason the editor's
    // demonstrator node types go through `add_builtin_node_type`: a contribution that bypassed the ledger would
    // be the one thing `origin_of` could not answer for, and the one thing a session could not take back.
    //
    // Mounted **before** the window is built, because the window's measurement panel reads the registry during
    // construction -- a device registered afterwards would be missing from the list until something refreshed it,
    // and nothing does.
    //
    // The count is reported rather than asserted. A device whose id was taken is one device missing from the rack,
    // which is a usable instrument list with something absent; refusing the whole rack would turn a name clash
    // into "this build cannot measure anything at all".
#if defined(QP_HAS_INSTRUMENTS_PLUGIN)
    const std::size_t mounted_devices = qp::plugins::instruments::mount_instruments(content_host);
    if (mounted_devices != qp::plugins::instruments::builtin_count()) {
        qWarning().noquote() << "instruments: mounted" << mounted_devices << "of"
                             << qp::plugins::instruments::builtin_count();
    }
#endif

    // The magnetosphere kit's node types: the field model, the emitter and the pusher.
    //
    // **Mounted even though the Run action cannot drive them yet**, and the alternative was worse: a kit whose
    // types are registered only by a test fixture is a kit no user can reach, which is the failure
    // `docs/plan-tree.md` section 9.9 calls out by name -- "a capability only a test fixture can reach looks
    // green in every check". What a user *can* do with them today is real: place a dipole, wire a ring emitter
    // and a Boris push to it, set the pitch angle and the L shell, and save the document. What they cannot do is
    // press Run, because the particle domain is driven by `MagnetosphereRun` -- a run path the view layer does
    // not have -- and the Run action says exactly that rather than starting something else.
    //
    // The count is reported rather than asserted, for the reason the instruments' is: a name clash is one
    // palette entry missing, not a build that cannot open a window.
#if defined(QP_HAS_MAGNETOSPHERE_PLUGIN)
        qp::plugins::magnetosphere::MagnetosphereRunProvider magnetosphere_run;
    // The kit's whole-graph run provider. The operator loop drives one operator over a state of a few doubles
    // per particle; this kit's graph has to be baked and launched first, so pressing Run on it goes through
    // `IGraphRunProvider` instead -- see `views/model/run_providers.hpp`.
    // The drawing side of the same kit: one item per render type it declares. Mounted beside the run provider,
    // and the window offers every declaration to every item and keeps one panel per item -- which is why there
    // are two here and not one: `render.particles` and `render.field_lines` are different declarations, and an
    // item that claimed both would have to choose which picture to draw.
    qp::plugins::magnetosphere::ParticleViewItem particle_item;
    qp::graph::mount_view_item(&particle_item);
    qp::plugins::magnetosphere::FieldLinesViewItem field_lines_item;
    qp::graph::mount_view_item(&field_lines_item);
    qp::views::model::mount_run_provider(&magnetosphere_run);
    const std::size_t mounted_field_types = qp::plugins::magnetosphere::FieldNodes::mount(content_host);
    const std::size_t mounted_pushers = qp::plugins::magnetosphere::PusherNodes::mount(content_host);
    const std::size_t mounted_emitters = qp::plugins::magnetosphere::EmitterNodes::mount(content_host);
    const std::size_t mounted_render_items = qp::plugins::magnetosphere::RenderNodes::mount(content_host);
    // **Each count is compared against its own expected number**, and the first version of this check is why: it
    // summed the three field-side counts and compared the total against a literal 3, so adding the electric field
    // node made a healthy application print `mounted 6 of 3 node types` on every launch. The literal had been
    // right when it was written and nothing tied it to the kit it was counting, which is the same shape as the
    // stale expectations this repository keeps finding in tests -- a number that describes another module's
    // inventory does not live in this one.
    if (mounted_field_types != 11 || mounted_pushers != 1 || mounted_emitters != 1) {
        qWarning().noquote() << "magnetosphere: mounted" << mounted_field_types << "of 11 field types,"
                             << mounted_pushers << "of 1 pusher and" << mounted_emitters << "of 1 emitter";
    }
    if (mounted_render_items != 2) {
        qWarning().noquote() << "magnetosphere: mounted" << mounted_render_items << "of 2 render items";
    }
#endif

    qp::views::EditorWindow window(content_host);
    // The application decides to seed itself; the window does not. One call, so the graph and
    // the measurement session cannot be seeded in the wrong order by a caller that only
    // remembered one of them.
    window.seed_demo();
    window.show();
    // The host outlives the window: it owns the catalog the window resolved types against, and a plugin
    // unloaded while the window still held a descriptor pointer would be the dangling case the host's own
    // record exists to prevent.
    return QApplication::exec();
}
