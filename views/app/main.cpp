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
#include <qp/plugins/magnetosphere/source_nodes.hpp>
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
    // **This comment used to say the Run action could not drive them, and that had been false for a long time.**
    // The claim was that "the particle domain is driven by `MagnetosphereRun` -- a run path the view layer does not
    // have"; the path exists (the controller asks the mounted providers after the operator path declines, and the
    // run provider is mounted a few lines below), and the measured answer to pressing Run on the flagship graph is
    // **"boris: 4096 steps, 24 of 24 live"** with the particles and the field lines drawn. The passage is kept in
    // outline because its argument about *why* mounting matters is still right, and because it is an example of the
    // one kind of comment that goes stale on its own: a claim about what a capability can do.
    //
    // `run.controller.a_content_graph_runs_through_the_provider` asserts the path now -- the provider's name in the
    // report, the sixteen particles, the four channels with `radius` in metres, the ledger entry, and the refusal
    // when the provider is absent -- so the two comments here and in `views/CMakeLists.txt` have something to be
    // checked against.
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
    // The demos this build offers, handed to the window at construction. Declared here rather than inside the
    // plugin guard so the window's constructor has one argument either way.
    std::vector<qp::views::model::GraphBlueprint> demos;

    qp::plugins::magnetosphere::ParticleViewItem particle_item;
    qp::graph::mount_view_item(&particle_item);
    qp::plugins::magnetosphere::FieldLinesViewItem field_lines_item;
    qp::graph::mount_view_item(&field_lines_item);
    qp::views::model::mount_run_provider(&magnetosphere_run);
    const std::size_t mounted_field_types = qp::plugins::magnetosphere::FieldNodes::mount(content_host);
    const std::size_t mounted_pushers = qp::plugins::magnetosphere::PusherNodes::mount(content_host);
    const std::size_t mounted_emitters = qp::plugins::magnetosphere::EmitterNodes::mount(content_host);
    const std::size_t mounted_render_items = qp::plugins::magnetosphere::RenderNodes::mount(content_host);
    const std::size_t mounted_drivers = qp::plugins::magnetosphere::SourceNodes::mount(content_host);
    // **Each count is compared against its own expected number**, and the first version of this check is why: it
    // summed the three field-side counts and compared the total against a literal 3, so adding the electric field
    // node made a healthy application print `mounted 6 of 3 node types` on every launch. The literal had been
    // right when it was written and nothing tied it to the kit it was counting, which is the same shape as the
    // stale expectations this repository keeps finding in tests -- a number that describes another module's
    // inventory does not live in this one.
    if (mounted_field_types != 17 || mounted_pushers != 3 || mounted_emitters != 1 || mounted_drivers != 2) {
        qWarning().noquote() << "magnetosphere: mounted" << mounted_field_types << "of 17 field types,"
                             << mounted_pushers << "of 2 pushers," << mounted_emitters << "of 1 emitter and"
                             << mounted_drivers << "of 2 drivers";
    }
    if (mounted_render_items != 2) {
        qWarning().noquote() << "magnetosphere: mounted" << mounted_render_items << "of 2 render items";
    }

    // **The flagship composition, as a blueprint rather than as window code.** A dipole, the tail's current
    // sheet, their sum, a magnetosheath field, the magnetopause weight, and the mix that puts them together; the
    // two drivers that decide the boundary's standoff and the dipole's tilt; the shielded convection field; then
    // an emitter launched against that field, a Boris push, and the two render declarations the kit's view items
    // claim. It is the picture the reference exists to draw, and it is expressed in the plugin's own vocabulary
    // here because this file is the composition root: the window is handed the blueprint and knows none of these
    // names.
    //
    // The grid is one earth radius on a forty-one-node box: coarse enough that five field tables cost a few
    // megabytes, fine enough that the dipole's gradient across a cell is visible in the particles.
    {
        const double re = qp::plugins::magnetosphere::kEarthRadiusM;
        qp::views::model::GraphBlueprint flagship;
        flagship.label = "magnetosphere";
        const auto node = [&flagship](const char* type, const char* name) {
            qp::views::model::BlueprintNode n;
            n.type_name = type;
            n.name = name;
            flagship.nodes.push_back(std::move(n));
            return flagship.nodes.size() - 1;
        };
        const auto set = [&flagship](std::size_t index, qp::graph::PortNumber port, qp::ports::Value value) {
            flagship.nodes[index].params.emplace_back(port, std::move(value));
        };
        const auto grid = [&set](std::size_t index, qp::graph::PortNumber first) {
            for (qp::graph::PortNumber axis = 0; axis < 3; ++axis) {
                set(index, first + axis, qp::ports::Value{-20.0 * qp::plugins::magnetosphere::kEarthRadiusM});
                set(index, first + 3 + axis, qp::ports::Value{1.0 * qp::plugins::magnetosphere::kEarthRadiusM});
                set(index, first + 6 + axis, qp::ports::Value{41.0});
            }
        };

        using qp::plugins::magnetosphere::EmitterNodes;
        using qp::plugins::magnetosphere::FieldNodes;
        using qp::plugins::magnetosphere::PusherNodes;
        using qp::plugins::magnetosphere::RenderNodes;
        using qp::plugins::magnetosphere::SourceNodes;

        const std::size_t dipole = node(FieldNodes::kDipoleType, "dipole");
        set(dipole, FieldNodes::kPortTiltDegrees, qp::ports::Value{0.0});
        set(dipole, FieldNodes::kPortMomentAm2, qp::ports::Value{qp::plugins::magnetosphere::kDipoleMomentAm2});
        grid(dipole, FieldNodes::kPortOrigin0);

        // The tail: the reference's own composition, which is `model = flaring` with an index on the socket. The two
        // parameters below are what stands when **no** wire is present -- a lobe field of five nanotesla and a
        // half-thickness of two earth radii -- and the index replaces all three of the tail's numbers at once
        // (including the northward component the reference calls `Bz0`), because they are one model's answer for one
        // day. At Kp 4 that is a fifty-nanotesla lobe field and a 1.5-earth-radius sheet: the realistic tail, and
        // four times the amplitude the parameter alone would give.
        const std::size_t sheet = node(FieldNodes::kCurrentSheetType, "tail sheet");
        set(sheet, FieldNodes::kPortSheetB0, qp::ports::Value{5.0e-9});
        set(sheet, FieldNodes::kPortSheetThickness, qp::ports::Value{2.0 * re});
        set(sheet, FieldNodes::kPortSheetModel, qp::ports::Value{std::int64_t{1}});   // the flaring profile
        grid(sheet, FieldNodes::kPortSheetOrigin0);

        const std::size_t sum = node(FieldNodes::kSumType, "dipole + sheet");
        grid(sum, FieldNodes::kPortOrigin0);

        // The external field: **the modelled IMF rather than three typed numbers.** This is the porting ledger's
        // reopen condition for `imf_source`, and it makes the composition the reference's "magnetopause + uniform
        // IMF" arrangement with the solar wind's own spiral angle instead of a vector somebody chose. Its magnitude
        // rides the same Kp index the boundary does, so one number now strengthens the field *and* pulls the
        // magnetopause in -- which is the physical coupling the reference's `imf_source` exists to express.
        //
        // `field.uniform` is still in the palette -- it is the field a course checks a pusher against, and its place
        // in this demo is what has been replaced, not the type.
        const std::size_t sheath = node(FieldNodes::kImfType, "IMF");
        set(sheath, FieldNodes::kPortImfAngle, qp::ports::Value{FieldNodes::kDefaultImfAngleDegrees});
        grid(sheath, FieldNodes::kPortImfOrigin0);

        const std::size_t boundary = node(FieldNodes::kMagnetopauseType, "magnetopause");
        set(boundary, FieldNodes::kPortMagnetopauseStandoff, qp::ports::Value{10.0 * re});
        set(boundary, FieldNodes::kPortMagnetopauseFlaring, qp::ports::Value{0.58});
        set(boundary, FieldNodes::kPortMagnetopauseWidth, qp::ports::Value{1.0 * re});
        grid(boundary, FieldNodes::kPortMagnetopauseOrigin0);

        // The dayside draping: the reference's `mp_model = 2`, as a node rather than as a setting. It reads the
        // **boundary's own two tables** -- the radius it wraps around and the weight that says where it is a model --
        // so there is exactly one answer in this graph to "where is the magnetopause", and the wire is the agreement.
        const std::size_t draped = node(FieldNodes::kDrapeType, "draping");

        const std::size_t driver = node(SourceNodes::kKpType, "Kp");
        set(driver, SourceNodes::kPortKp, qp::ports::Value{4.0});

        // The second driver: **the June solstice**, the reference's own default date, and now the composition can
        // honestly show it. One date index becomes 11 + 23.44 = 34.44 degrees of dipole tilt, and the same wire
        // feeds the tail sheet's hinge below -- which is what makes it defensible. Until this round the sheet lay in
        // the z = 0 plane whatever the dipole did, so the largest tilt the demo could show without the two models
        // contradicting each other was about twelve degrees; the hinge is the piece that was missing, and it is why
        // this line used to say December.
        //
        // One index, two consumers, and neither of them is told how to convert it: the tilt arrives in degrees
        // because both ports say `deg`. The dipole and the sheet keep their own zero parameters, so deleting either
        // wire visibly changes the picture instead of silently reproducing it.
        const std::size_t date = node(SourceNodes::kDayType, "solstice");
        set(date, SourceNodes::kPortDay, qp::ports::Value{SourceNodes::kDefaultDay});

        const std::size_t mix = node(FieldNodes::kMixType, "inside + outside");
        set(mix, FieldNodes::kPortMixCorrection, qp::ports::Value{1.0});

        // The shielded convection field, as the reference composes it: `mul(convection, shield)` -- the
        // coefficient node's whole purpose, wired exactly the way `efield.py`'s own header shows.
        const std::size_t convection = node(FieldNodes::kConvectionType, "convection");
        for (qp::graph::PortNumber axis = 0; axis < 3; ++axis) {
            set(convection, FieldNodes::kPortConvectionOrigin0 + axis, qp::ports::Value{-20.0 * re});
            set(convection, FieldNodes::kPortConvectionOrigin0 + 3 + axis, qp::ports::Value{1.0 * re});
            set(convection, FieldNodes::kPortConvectionOrigin0 + 6 + axis, qp::ports::Value{41.0});
        }
        const std::size_t shield = node(FieldNodes::kShieldType, "shield");
        set(shield, FieldNodes::kPortShieldR0, qp::ports::Value{4.0 * re});
        for (qp::graph::PortNumber axis = 0; axis < 3; ++axis) {
            set(shield, FieldNodes::kPortShieldOrigin0 + axis, qp::ports::Value{-20.0 * re});
            set(shield, FieldNodes::kPortShieldOrigin0 + 3 + axis, qp::ports::Value{1.0 * re});
            set(shield, FieldNodes::kPortShieldOrigin0 + 6 + axis, qp::ports::Value{41.0});
        }
        const std::size_t shielded = node(FieldNodes::kMulType, "shielded E");

        const std::size_t emitter = node(EmitterNodes::kRingType, "ring");
        set(emitter, EmitterNodes::kPortSpecies, qp::ports::Value{std::int64_t{0}});
        set(emitter, EmitterNodes::kPortLShell, qp::ports::Value{6.6});
        set(emitter, EmitterNodes::kPortCount, qp::ports::Value{24.0});
        set(emitter, EmitterNodes::kPortBeta, qp::ports::Value{0.01});
        set(emitter, EmitterNodes::kPortPitchAngle, qp::ports::Value{90.0});
        set(emitter, EmitterNodes::kPortFlowAngle, qp::ports::Value{90.0});
        set(emitter, EmitterNodes::kPortRingSpan, qp::ports::Value{360.0});

        const std::size_t pusher = node(PusherNodes::kBorisType, "boris");
        set(pusher, PusherNodes::kPortMaxRangeRe, qp::ports::Value{20.0});

        const std::size_t particles = node(RenderNodes::kParticlesType, "particles");
        const std::size_t lines = node(RenderNodes::kFieldLinesType, "field lines");

        const auto wire = [&flagship](std::size_t from, qp::graph::PortNumber from_port, std::size_t to,
                                      qp::graph::PortNumber to_port) {
            qp::views::model::BlueprintWire w;
            w.from = from;
            w.from_port = from_port;
            w.to = to;
            w.to_port = to_port;
            flagship.wires.push_back(w);
        };
        wire(dipole, FieldNodes::kPortField, sum, FieldNodes::kPortAddendA);
        wire(sheet, FieldNodes::kPortField, sum, FieldNodes::kPortAddendB);
        wire(sum, FieldNodes::kPortField, mix, FieldNodes::kPortMixA);
        wire(boundary, FieldNodes::kPortWeight, mix, FieldNodes::kPortMixWeight);
        // The driver: one index that decides the boundary's standoff and flaring, rather than two numbers typed into
        // the boundary. This is what source.kp exists for, and the demo shows the wire.
        wire(driver, SourceNodes::kPortKpOut, boundary, FieldNodes::kPortMagnetopauseKp);
        // ... and the same index into the IMF's magnitude socket, so the storm that pulls the boundary in also
        // strengthens the field it is standing in.
        wire(driver, SourceNodes::kPortKpOut, sheath, FieldNodes::kPortImfKp);
        // ... and into the tail's, which is the third consumer: one index now decides the boundary, the solar wind
        // and the tail at once, which is what the reference's three `*_source` nodes exist to express.
        wire(driver, SourceNodes::kPortKpOut, sheet, FieldNodes::kPortSheetKp);
        // The tilt socket, which is optional by construction: `field.dipole` bakes its table from
        // `tilt_degrees` when nothing is wired there and from the socket when something is.
        wire(date, SourceNodes::kPortDayOut, dipole, FieldNodes::kPortTiltDriver);
        // ... and the same date into the sheet's hinge, which is the pair the reference's `tail.py` wires: one
        // driver, two nodes that must agree about which way is up.
        wire(date, SourceNodes::kPortDayOut, sheet, FieldNodes::kPortSheetHinge);
        // The external field goes **through the draping** before it reaches the mix: that is the difference between
        // mode 1 (a uniform IMF) and mode 2 (the IMF wrapped around the boundary), and it is one node in a wire.
        wire(sheath, FieldNodes::kPortField, draped, FieldNodes::kPortDrapeField);
        wire(boundary, FieldNodes::kPortMagnetopauseRadius, draped, FieldNodes::kPortDrapeRadius);
        wire(boundary, FieldNodes::kPortWeight, draped, FieldNodes::kPortDrapeWeight);
        wire(draped, FieldNodes::kPortDrapeOut, mix, FieldNodes::kPortMixB);
        wire(mix, FieldNodes::kPortMixOut, emitter, EmitterNodes::kPortMagnetic);
        wire(convection, FieldNodes::kPortField, shielded, FieldNodes::kPortMulField);
        wire(shield, FieldNodes::kPortShieldOut, shielded, FieldNodes::kPortMulWeight);
        wire(shielded, FieldNodes::kPortMulOut, pusher, PusherNodes::kPortElectric);
        wire(mix, FieldNodes::kPortMixOut, pusher, PusherNodes::kPortMagnetic);
        wire(emitter, EmitterNodes::kPortState, pusher, PusherNodes::kPortStateIn);
        wire(pusher, PusherNodes::kPortStateOut, particles, RenderNodes::kPortState);
        wire(mix, FieldNodes::kPortMixOut, lines, RenderNodes::kPortField);

        // **Checked before it is offered.** A demo this build cannot assemble is reported here, at startup, with
        // the sentence the check produced -- rather than appearing in the menu and failing halfway through.
        const qp::views::model::BlueprintCheck offered =
            qp::views::model::check_blueprint(content_host.node_types(), qp::ports::builtin_registry(), flagship);
        if (!offered.ok) {
            qWarning().noquote() << "magnetosphere demo cannot be offered:" << offered.refusal.c_str();
        }
        // **The standard dipole, beside the flagship.** The question that produced this preset was whether the
    // magnetosphere picture is a dipole -- and it is not, by construction: the flagship adds a tail current sheet, a
    // magnetopause shield, a draping layer and a mix, so its lines are compressed on the day side and swept back on
    // the night side. Those are the right physics for a magnetosphere and the wrong thing to compare a dipole
    // against. This is the comparison: one centred dipole at zero tilt, and the field-line declaration that draws
    // it -- `r = L sin^2(theta)`, the closed form
    // `magnetosphere.trace.every_point_of_a_dipole_line_is_the_closed_form` asserts point by point (worst relative
    // deviation 0.0097 over six shells, which is the 0.25 R_E lattice and not the model).
    {
        using qp::plugins::magnetosphere::FieldNodes;
        using qp::plugins::magnetosphere::RenderNodes;
        qp::views::model::GraphBlueprint standard;
        standard.label = "dipole (standard)";
        const auto node = [&standard](const char* type, const char* name) {
            qp::views::model::BlueprintNode n;
            n.type_name = type;
            n.name = name;
            standard.nodes.push_back(std::move(n));
            return standard.nodes.size() - 1;
        };
        const auto set = [&standard](std::size_t index, qp::graph::PortNumber port, qp::ports::Value value) {
            standard.nodes[index].params.emplace_back(port, std::move(value));
        };
        const auto wire = [&standard](std::size_t from, qp::graph::PortNumber from_port, std::size_t to,
                                      qp::graph::PortNumber to_port) {
            qp::views::model::BlueprintWire w;
            w.from = from;
            w.from_port = from_port;
            w.to = to;
            w.to_port = to_port;
            standard.wires.push_back(w);
        };

        const std::size_t field = node(FieldNodes::kDipoleType, "dipole");
        set(field, FieldNodes::kPortTiltDegrees, qp::ports::Value{0.0});
        set(field, FieldNodes::kPortMomentAm2, qp::ports::Value{qp::plugins::magnetosphere::kDipoleMomentAm2});
        // The flagship's own lattice, so the two pictures differ in the *field* and not in the sampling.
        for (qp::graph::PortNumber axis = 0; axis < 3; ++axis) {
            set(field, static_cast<qp::graph::PortNumber>(FieldNodes::kPortOrigin0 + axis),
                qp::ports::Value{-8.0 * re});
            set(field, static_cast<qp::graph::PortNumber>(FieldNodes::kPortSpacing0 + axis),
                qp::ports::Value{0.25 * re});
            set(field, static_cast<qp::graph::PortNumber>(FieldNodes::kPortCount0 + axis),
                qp::ports::Value{65.0});
        }

        const std::size_t lines = node(RenderNodes::kFieldLinesType, "lines");
        set(lines, RenderNodes::kPortLineCount, qp::ports::Value{std::int64_t{7}});
        set(lines, RenderNodes::kPortSeedStart, qp::ports::Value{2.0});
        set(lines, RenderNodes::kPortSeedEnd, qp::ports::Value{8.0});
        wire(field, FieldNodes::kPortField, lines, RenderNodes::kPortField);
        demos.push_back(std::move(standard));
    }

    demos.push_back(std::move(flagship));
    }
#endif

    qp::views::EditorWindow window(content_host, demos);
    // The application decides to seed itself; the window does not. One call, so the graph and
    // the measurement session cannot be seeded in the wrong order by a caller that only
    // remembered one of them.
    //
    // `--demo <label>` opens a demo this build offers instead of the default experiment. It exists for two
    // reasons: a course can launch straight into the kit it cares about, and the flagship composition below can be
    // verified without a synthetic click -- a window that has to be clicked is a window whose screenshots prove
    // less.
    const QStringList arguments = QCoreApplication::arguments();
    const int demo_index = arguments.indexOf(QStringLiteral("--demo"));
    bool seeded_a_demo = false;
    if (demo_index >= 0 && demo_index + 1 < arguments.size()) {
        const QString wanted = arguments.at(demo_index + 1);
        for (const qp::views::model::GraphBlueprint& blueprint : demos) {
            if (QString::fromStdString(blueprint.label) != wanted) continue;
            window.seed_blueprint(blueprint);
            seeded_a_demo = true;
            break;
        }
        if (!seeded_a_demo) {
            qWarning().noquote() << "no demo named" << wanted << "-- this build offers" << demos.size();
        }
    }
    if (!seeded_a_demo) window.seed_demo();
    window.show();
    // The host outlives the window: it owns the catalog the window resolved types against, and a plugin
    // unloaded while the window still held a descriptor pointer would be the dangling case the host's own
    // record exists to prevent.
    return QApplication::exec();
}
