/**
 * @file editor_window.cpp
 * @brief Implementation of the editing window.
 */
#include "editor_window.hpp"

#include "node_graph_view.hpp"
#include <QAction>
#include <QToolBar>

#include <qp/views/model/execution_binders.hpp>

#include "confidence_panel.hpp"
#include "measurement_panel.hpp"
#include "property_panel.hpp"

#include <qp/graph/mutate/command.hpp>
#include <qp/ports/value.hpp>

#include <QHBoxLayout>
#include <QLabel>
#include <QDockWidget>
#include <QListWidget>
#include <QSplitter>
#include <QStatusBar>
#include <QString>
#include <QVBoxLayout>
#include <QTimer>
#include <QWidget>

#include <string>

namespace qp::views {
namespace {

/// @brief Initial width of the measurement dock, in logical pixels.
///
/// Wide enough for the readings table's four columns at QFont's default size, and no wider: the
/// canvas is the panel that cannot do its job without width, and a dock that takes more than it
/// needs takes it from the canvas.
constexpr int kDockWidth = 320;

}  // namespace

namespace {

/// @brief An error code as text, for the status line.
QString describe(qp::diag::ErrorCode code) {
    const std::string_view text = qp::diag::to_string(code);
    return QString::fromLatin1(text.data(), static_cast<int>(text.size()));
}

}  // namespace

/// @brief Keeps the status line in step with the session.
class EditorWindow::StatusBridge final : public qp::authoring::IChangeListener {
public:
    explicit StatusBridge(EditorWindow& window) noexcept : window_(&window) {}
    void on_change(const qp::authoring::Change&) noexcept override {
        window_->refresh_status();
    }

private:
    EditorWindow* window_;
};

EditorWindow::EditorWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("qp -- experiment editor"));

    // The built-in demonstrators, so the palette and the panel have something to
    // resolve. A real build loads these from plugins; the shell registers them
    // directly, which is the only thing the shell is allowed to shortcut.
    const auto seeded = register_demo_library(catalog_);
    if (!seeded.has_value()) {
        // Not fatal: an incomplete library still gives a usable window, and the
        // status line will name the missing types once it is built.
    }

    canvas_ = new NodeGraphView(session_, catalog_, document_, this);
    properties_ = new PropertyPanel(session_, catalog_, port_ui_, this);
    // Without a minimum the splitter collapses this panel to nothing when the
    // other two want more room -- and the first screenshot of this window showed
    // the panel simply absent, which reads as a missing feature rather than as a
    // layout problem.
    properties_->setMinimumWidth(280);
    // The splitter hands out space by stretch factor, and a panel with stretch 0
    // and no size hint gets none of it. The canvas is the part that should absorb
    // the slack, so the panel is given an explicit initial width instead.
    properties_->resize(300, properties_->height());

    auto* palette = new QListWidget(this);
    palette->setMinimumWidth(180);
    for (const qp::graph::NodeDesc* desc : catalog_.all()) {
        palette->addItem(QString::fromStdString(desc->label.empty() ? desc->type_name
                                                                   : desc->label));
    }
    connect(palette, &QListWidget::itemDoubleClicked, this, [this, palette](QListWidgetItem* it) {
        const auto index = static_cast<std::size_t>(palette->row(it));
        const auto all = catalog_.all();
        if (index < all.size()) {
            // Through add_node, which goes through the session. The palette is not
            // allowed a shortcut the canvas does not get.
            (void)add_node(all[index]->type_name);
        }
    });

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(palette);
    splitter->addWidget(canvas_);
    splitter->addWidget(properties_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    // Explicit widths, applied once the window has been laid out. `setSizes` called during
    // construction is immediately overwritten when the splitter is first resized to fit the
    // window, which is how the property panel ended up with no width at all -- and a panel that
    // is not visible reads as a missing feature rather than as a layout problem.
    //
    // The numbers are chosen to **fit**, and that is a correction rather than a preference. The
    // previous request was `{200, 680, 340}`: 1320 logical pixels. The central widget is about
    // 860 of the window's 1280 once the right-side measurement dock takes its share, and Qt
    // honours a splitter's ratios rather than its absolute sizes -- so every panel came out at
    // roughly two thirds of what it asked for. The canvas, which is the one panel that needs
    // width, lost the most: it was left a narrow strip, and `fitInView` then shrank three nodes
    // until their labels could not be read. Asking for less than is available is the fix;
    // asking for more and being scaled is the bug.
    QTimer::singleShot(0, this, [splitter] { splitter->setSizes({170, 430, 260}); });

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(splitter);
    setCentralWidget(central);

    measurements_panel_ = new MeasurementPanel(measurements_, this);
    auto* dock = new QDockWidget(tr("Measurement"), this);
    dock->setWidget(measurements_panel_);
    // Not closable and not floatable. The panel is the reporting half of the loop the
    // platform exists for, and a window that can hide it can present a measurement session
    // with no uncertainty visible -- which is precisely the artefact being replaced.
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    confidence_panel_ = new ConfidencePanel(confidence_, this);
    auto* confidence_dock = new QDockWidget(tr("Confidence"), this);
    confidence_dock->setWidget(confidence_panel_);
    confidence_dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    // Stacked below the measurement dock rather than floated elsewhere. The two are read together:
    // the readings say what was measured, and this says whether the numbers behind them can be
    // trusted. Putting them in different corners is how a user reads one and not the other.
    addDockWidget(Qt::RightDockWidgetArea, confidence_dock);
    splitDockWidget(dock, confidence_dock, Qt::Vertical);

    // An explicit width, applied after the first layout pass. A table of readings has no opinion
    // about how wide it should be, and letting `sizeHint` decide is how the dock took 420 of the
    // window's 1280 while the canvas had 185 and could not show the graph it was drawing.
    QTimer::singleShot(0, this, [this, dock] {
        resizeDocks({dock}, {kDockWidth}, Qt::Horizontal);
    });

    // The Run action. Built after the panels, because `run_once` refreshes them.
    // The binders come from wherever the application mounted them. An empty list is a legitimate
    // state -- a build with no plugins has none -- and the Run action's message for that case is
    // already true and actionable.
    run_controller_ = std::make_unique<qp::views::model::RunController>(
        session_, qp::views::model::execution_binders());
    auto* tools = addToolBar(tr("Experiment"));
    tools->setMovable(false);
    run_action_ = tools->addAction(tr("Run"));
    run_action_->setToolTip(QString::fromStdString(run_controller_->description()));
    run_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    connect(run_action_, &QAction::triggered, this, &EditorWindow::run_once);

    status_ = new QLabel(this);
    statusBar()->addWidget(status_);

    // The canvas tells the panel what to show. Note the direction: the panel does
    // not ask the canvas, and neither asks the graph. One signal, and both keep
    // reading the same session.
    connect(canvas_, &NodeGraphView::node_selected, this, &EditorWindow::on_node_selected);
    connect(canvas_, &NodeGraphView::mutation_failed, this, &EditorWindow::on_mutation_failed);
    connect(properties_, &PropertyPanel::edit_failed, this, &EditorWindow::on_mutation_failed);

    // The status line follows the session, not just the edits this window starts.
    // A line that only updated on the paths this class knows about would go stale
    // the moment a panel changed something -- and a stale count is how a user
    // stops trusting the whole window.
    status_bridge_ = std::make_unique<StatusBridge>(*this);
    (void)session_.add_listener(*status_bridge_);

    refresh_status();
    resize(1280, 720);
}

void EditorWindow::seed_demo() {
    // Order is the whole content of this function. See the header.
    seed_demo_graph();
    seed_demo_measurement();
    refresh_status();
}

EditorWindow::~EditorWindow() = default;

qp::graph::NodeId EditorWindow::add_node(const std::string& type_name) {
    if (catalog_.find(type_name) == nullptr) {
        on_mutation_failed(QStringLiteral("no such type: %1")
                               .arg(QString::fromStdString(type_name)));
        return {};
    }

    const auto reserved = session_.reserve_node();
    if (!reserved.has_value()) {
        on_mutation_failed(describe(reserved.error()));
        return {};
    }

    qp::graph::AddNode command;
    command.id = reserved.value();
    command.type_name = type_name;
    command.name = "n" + std::to_string(next_node_index_++);

    const auto applied = session_.apply(command);
    if (!applied.has_value()) {
        on_mutation_failed(describe(applied.error()));
        return {};
    }

    refresh_status();
    return reserved.value();
}

void EditorWindow::seed_demo_graph() {
    // A small graph that connects, so the canvas has an edge to draw and the
    // panel has a node with parameters. Chosen to exercise the paths: a source
    // with an enum and bounded numbers, a model with an unbounded parameter, and
    // an instrument whose resolution is what turns a reading into an uncertainty.
    const qp::graph::NodeId source = add_node("demo.signal");
    const qp::graph::NodeId model = add_node("demo.spring_damper");
    const qp::graph::NodeId scope = add_node("demo.instrument");
    if (!source.valid() || !model.valid() || !scope.valid()) return;

    // The model's parameters, set through the session like every other edit -- so they show up in the
    // property panel, are undoable, and bump the graph version the run record pins.
    //
    // Without this the Run action reports "no node in this graph has an operator yet", which is true:
    // the binder needs `k`, `m`, `c` and `integrator`, and a node nobody has filled in is a node it
    // cannot run. That was the first thing the Run button did on launch, and it is the correct answer to
    // an incomplete demo rather than a defect in the run loop.
    //
    // `omega = sqrt(k/m) = 20 rad/s`, matching `RunController::kOmega` so the trace's frequency
    // component and the kernel's parameter block agree. Damping is zero **on purpose**: the kernel
    // integrates `x'' = -omega^2 x` and has no damping term, and the binder refuses a non-zero `c` rather
    // than silently dropping it. Seeding a non-zero value would open the demo on a refusal, which
    // teaches the wrong thing about what the tool can do.
    const auto set_parameter = [this](qp::graph::NodeId node, qp::graph::PortNumber port,
                                      qp::ports::Value value) {
        qp::graph::SetParam command;
        command.id = node;
        command.port = port;
        command.value = std::move(value);
        const auto applied = session_.apply(command);
        if (!applied.has_value()) on_mutation_failed(describe(applied.error()));
    };
    set_parameter(model, 2, qp::ports::Value{200.0});                     // stiffness, N/m
    set_parameter(model, 3, qp::ports::Value{0.0});                       // damping, N*s/m
    set_parameter(model, 4, qp::ports::Value{0.5});                       // mass, kg
    set_parameter(model, 5, qp::ports::Value{std::int64_t{1}});           // integrator: rk4

    // Connections go through the session too, so undo reverses them and both
    // panels are told. A failure is reported rather than swallowed: a graph that
    // silently lost an edge would look like a layout problem.
    const auto connect_ports = [this](qp::graph::NodeId from, qp::graph::NodeId to) {
        qp::graph::Connect command;
        command.from = qp::graph::PortRef{from, 1, qp::graph::PortDirection::output};
        command.to = qp::graph::PortRef{to, 1, qp::graph::PortDirection::input};
        const auto applied = session_.apply(command);
        if (!applied.has_value()) on_mutation_failed(describe(applied.error()));
    };
    connect_ports(source, model);
    connect_ports(model, scope);

    // A run, so the status line reports something real rather than a placeholder.
    //
    // ## This record is deliberately incomplete, and that is the honest choice
    //
    // The window pins what it genuinely knows: the seed, the graph version, the toolchain, the
    // optimisation level and the time. It does **not** pin the parameter set or the plugin
    // versions, because it has neither -- this build loads no plugins, and no parameter set has
    // been chosen to record.
    //
    // The alternative is to write plausible values into the empty fields so the status line
    // reads "reproducibility gaps: none". That would be a lie of exactly the kind the run ledger
    // exists to prevent. A record that **claims** reproducibility while missing its inputs is
    // worse than one that admits the gap: the first sends a student hunting for the discrepancy
    // inside their own physics, while the second tells them to re-run on the same machine.
    //
    // So the gap list is a to-do list for whoever deploys this, not a defect. A build with
    // plugins loaded drops `plugin_versions` from it; a course that pins its compiler drops
    // `toolchain`. Nothing here should be made to disappear by editing this function.
    qp::runtime::RunSpec spec;
    spec.seed = 20260911;
    spec.graph_version = session_.graph().version();
    spec.toolchain = qp::runtime::toolchain_id();
    spec.optimisation = qp::runtime::optimisation_id();
    spec.started_at = qp::runtime::now_unix_seconds();
    spec.fields = qp::runtime::ReproField::seed | qp::runtime::ReproField::graph_version |
                  qp::runtime::ReproField::toolchain | qp::runtime::ReproField::optimisation;
    (void)ledger_.begin(std::move(spec));

    refresh_status();
}

void EditorWindow::on_node_selected(qp::graph::NodeId node) {
    properties_->show_node(node);
}

void EditorWindow::on_mutation_failed(const QString& reason) {
    status_->setText(QStringLiteral("refused: %1").arg(reason));
}

void EditorWindow::refresh_status() {
    // Every number is read from the core. A status line with a hard-coded value
    // would keep looking correct after the wiring behind it had broken.
    const std::string gaps = [this] {
        if (ledger_.records().empty()) return std::string{"no run yet"};
        const std::vector<std::string> missing = ledger_.records().back().spec.missing_names();
        if (missing.empty()) return std::string{"complete"};
        std::string text;
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i != 0) text += ", ";
            text += missing[i];
        }
        return text;
    }();

    status_->setText(QStringLiteral("nodes %1 | edges %2 | graph v%3 | changes %4 | undo %5 | "
                                    "runs %6 | reproducibility gaps: %7")
                         .arg(session_.graph().node_count())
                         .arg(session_.graph().edge_count())
                         .arg(session_.graph().version())
                         .arg(session_.sequence())
                         .arg(session_.can_undo() ? "yes" : "no")
                         .arg(ledger_.size())
                         .arg(QString::fromStdString(gaps)));
}

void EditorWindow::build_palette() {
    // Reserved for when the palette becomes a view of its own; the constructor
    // currently fills it inline. Present so the intent is recorded rather than the
    // filling being scattered.
}

void EditorWindow::seed_demo_measurement() {
    namespace rt = qp::runtime;
    using rt::UncertaintyKind;

    // A repeat measurement of one length, in metres. The two quantified readings carry the
    // scale's resolution as their uncertainty; the third is a reading whose error nobody
    // worked out -- the normal state of a lab notebook, and the case the panel must not
    // round to zero.
    measurements_.add_reading(0.0241, UncertaintyKind::standard, 0.0005);
    measurements_.add_reading(0.0238, UncertaintyKind::standard, 0.0005);
    measurements_.add_reading(0.0243, UncertaintyKind::unknown);

    // A short trace, so the time axis is exercised rather than merely present.
    //
    // **Two channels, and the velocity is not optional.** The confidence panel's energy diagnostic
    // needs a position and a velocity: a quadratic potential is `0.5 w^2 x^2 + 0.5 v^2`, and with
    // only the first the panel reports "energy drift cannot be measured: the trace is missing
    // velocity". That is the honest answer and it was the demo's answer until this channel was
    // added -- which meant the demo showed C8's **refusal** path and never its diagnostic.
    //
    // The velocity is the analytic derivative of the position rather than a second independent
    // series, so the two channels describe one motion. The functions are
    //
    //     x(t) = A e^{-g t} cos(w t)
    //     v(t) = A e^{-g t} (-g cos(w t) - w sin(w t))
    //
    // with `A = 0.02`, `g = 0.8`, `w = 12`. The amplitude decays by construction, so the total
    // energy decays too, and the panel's note names the mechanism: this **model** has damping. That
    // is deliberately not the RK4 dissipation case -- the two are different findings, and the note
    // distinguishes them ("a property of the integrator and not of the model unless the model has
    // damping"). A demo whose energy were perfectly conserved would show the panel agreeing and
    // never show it disagreeing, which is the less useful half.
    (void)measurements_.add_channel("displacement", qp::units::dims::length);
    (void)measurements_.add_channel("velocity", qp::units::dims::velocity);
    constexpr double kAmplitude = 0.02;
    constexpr double kDecay = 0.8;
    constexpr double kOmega = 12.0;
    for (int i = 0; i < 200; ++i) {
        const double t = 0.01 * static_cast<double>(i);
        const double envelope = kAmplitude * std::exp(-kDecay * t);
        const double x = envelope * std::cos(kOmega * t);
        const double v = envelope * (-kDecay * std::cos(kOmega * t) - kOmega * std::sin(kOmega * t));
        (void)measurements_.add_sample(t, std::vector<double>{x, v}, 0.0005);
    }

    // No run is recorded here. `seed_demo_graph` already recorded one, and a second record
    // describing nothing would make the ledger's own report describe neither -- its counts
    // and its gap list are about **the last run**, so a second record means the run being
    // described is whichever happened to be second.
    //
    // The trace above therefore belongs to the run `seed_demo_graph` opened. That is the
    // honest arrangement rather than a shortcut: a trace is the record of one run, and this
    // window's is the one it already started.

    measurements_panel_->refresh();

    // Declare the frequency the energy diagnostic should assume, and refresh the panel.
    //
    // The seed's trace is a decaying oscillation rather than a solution of the RK4 operator, so
    // there is no operator whose `clamps_fired()` could be read -- and inventing a count would make
    // the panel assert something about a run that did not happen. `note_clamps` is therefore left
    // at its zero default, which is the truth here: nothing was clamped because nothing integrated.
    // The demo's own frequency, so the energy figure is computed for the potential the data came
    // from. Declaring a different value would produce a drift that is an artefact of the mismatch,
    // and the panel would be reporting its own input as a finding about the run.
    confidence_.set_omega(kOmega);
    clamps_noted_ = true;
    confidence_panel_->refresh();
}


void EditorWindow::run_once() {
    if (run_controller_ == nullptr) return;

    qp::views::model::RunResult result = run_controller_->run();

    // The status line speaks first: it is where every other message in this window goes, and a user who
    // pressed a button is looking for a response in one consistent place.
    status_->setText(QString::fromStdString(result.report.message));

    if (!result.report.ok) {
        // A failed run leaves the panels alone. Replacing good readings with nothing because a run was
        // refused would destroy the user's work to report a problem that did not touch it.
        //
        // An empty trace after a *successful* run is not possible; after a failed one it means the
        // binder was never reached, and there is nothing to show either way.
        if (result.trace.empty()) return;
        // A **partial** trace is shown: the confidence panel exists to make a diverging run visible, and
        // it cannot do that from an empty panel.
    }

    // The measurement session is **replaced**, not appended to. The seeded demo describes a different
    // experiment from the one that just ran, and appending would make the time axis go backwards at the
    // seam -- which the trace refuses, so the failure would appear as an append error rather than as
    // "you ran a new experiment".
    measurements_.reset_trace(result.report.run);

    // The two channels are copied across by name, so the panels find them through the same constants the
    // run loop wrote them with. A rename on either side breaks here rather than silently producing an
    // unaskable trace.
    for (const qp::runtime::Channel& channel : result.trace.channels()) {
        (void)measurements_.add_channel(channel.name, channel.dim);
    }
    for (const qp::runtime::Sample& sample : result.trace.samples()) {
        std::vector<double> values;
        values.reserve(sample.values.size());
        for (const qp::runtime::UncertainValue& v : sample.values) values.push_back(v.value);
        (void)measurements_.add_sample(sample.t, values, 0.0);
    }

    // The confidence model needs the frequency the run integrated against, or its energy figure would be
    // computed for a different potential than the data came from -- and it would then report drift that
    // is an artefact of the mismatch. `omega_was_declared` is what lets the panel say which it is.
    confidence_.set_omega(qp::views::model::RunController::kOmega);

    measurements_panel_->refresh();
    confidence_panel_->refresh();
    refresh_status();
}

}  // namespace qp::views
