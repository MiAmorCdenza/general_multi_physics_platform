/**
 * @file editor_window.cpp
 * @brief Implementation of the editing window.
 */
#include "editor_window.hpp"

#include "node_graph_view.hpp"
#include "property_panel.hpp"

#include <qp/graph/mutate/command.hpp>

#include <QHBoxLayout>
#include <QLabel>
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
    // Explicit widths, applied once the window has been laid out. `setSizes`
    // called during construction is immediately overwritten when the splitter is
    // first resized to fit the window, which is how the property panel ended up
    // with no width at all -- and a panel that is not visible reads as a missing
    // feature rather than as a layout problem.
    QTimer::singleShot(0, this, [splitter] { splitter->setSizes({200, 680, 340}); });

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(splitter);
    setCentralWidget(central);

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

}  // namespace qp::views
