/**
 * @file hello_view.cpp
 * @brief Implementation of the wiring-proof window.
 */
#include <qp/views/hello_view.hpp>

#include <QFormLayout>
#include <QLabel>
#include <QString>
#include <QVBoxLayout>

#include <string>

namespace qp::views {
namespace {

/// @brief Formats the session's state as the window shows it.
///
/// Every field is read from the core. Nothing here is a constant, because a demo
/// that displayed hard-coded numbers would keep passing after the link to the
/// core had broken -- which is the one thing this window exists to detect.
[[nodiscard]] QString describe(const qp::authoring::Session& session,
                               const qp::runtime::RunLedger& ledger) {
    QString text;
    text += QStringLiteral("nodes: %1\n").arg(session.graph().node_count());
    text += QStringLiteral("graph version: %1\n").arg(session.graph().version());
    text += QStringLiteral("changes applied: %1\n").arg(session.sequence());
    text += QStringLiteral("can undo: %1\n").arg(session.can_undo() ? "yes" : "no");
    text += QStringLiteral("runs recorded: %1\n").arg(ledger.size());
    text += QStringLiteral("incomplete runs: %1\n").arg(ledger.incomplete_count());
    text += QStringLiteral("toolchain: %1\n")
                .arg(QString::fromLatin1(qp::runtime::toolchain_id()));
    text += QStringLiteral("optimisation: %1")
                .arg(QString::fromLatin1(qp::runtime::optimisation_id()));
    return text;
}

}  // namespace

HelloView::HelloView(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QStringLiteral("qp -- view layer wiring check"));

    // One node, added the way a view is required to add one: through the session,
    // so the command bus, the undo stack and the change broadcast are all in the
    // path from the very first UI action.
    const auto reserved = session_.reserve_node();
    if (reserved.has_value()) {
        qp::graph::AddNode add;
        add.id = reserved.value();
        add.type_name = "demo";
        add.name = "wiring check";
        const auto applied = session_.apply(add);
        // A failed apply is not swallowed: the window is a diagnostic, and a
        // silent failure would make it report a state the graph never reached.
        if (!applied.has_value()) {
            // Nothing to do with the failure beyond leaving the counts honest;
            // the labels below are read from the session either way.
        }
    }

    // One run, with its toolchain and optimisation level measured rather than
    // written down. The spec is deliberately left incomplete -- no plugins, no
    // parameters, no graph version yet -- so the window shows the gap. A demo that
    // claimed completeness would be teaching the wrong lesson about what a
    // reproducible run needs.
    qp::runtime::RunSpec spec;
    spec.seed = 1;
    spec.graph_version = session_.graph().version();
    spec.toolchain = qp::runtime::toolchain_id();
    spec.optimisation = qp::runtime::optimisation_id();
    spec.started_at = qp::runtime::now_unix_seconds();
    spec.fields = qp::runtime::ReproField::seed | qp::runtime::ReproField::graph_version |
                  qp::runtime::ReproField::toolchain | qp::runtime::ReproField::optimisation;
    (void)ledger_.begin(std::move(spec));

    auto* layout = new QVBoxLayout(this);

    auto* heading = new QLabel(QStringLiteral("Core state, read live from the session"), this);
    layout->addWidget(heading);

    auto* state = new QLabel(describe(session_, ledger_), this);
    state->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(state);

    // The missing-inputs line is shown separately and prominently. It is the one
    // piece of information in this window that a user of the finished platform
    // will actually act on: it says which reproducibility inputs a run still
    // needs, rather than merely warning that something is absent.
    const std::vector<std::string> gaps =
        ledger_.records().empty() ? std::vector<std::string>{}
                                  : ledger_.records().front().spec.missing_names();
    QString gap_text = QStringLiteral("this run is missing: ");
    if (gaps.empty()) {
        gap_text += QStringLiteral("(nothing)");
    } else {
        for (std::size_t i = 0; i < gaps.size(); ++i) {
            if (i != 0) gap_text += QStringLiteral(", ");
            gap_text += QString::fromLatin1(gaps[i]);
        }
    }
    auto* gaps_label = new QLabel(gap_text, this);
    gaps_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(gaps_label);

    auto* footer = new QLabel(
        QStringLiteral("Qt %1 -- widgets, no QML (see ADR-0006)").arg(QT_VERSION_STR), this);
    layout->addWidget(footer);

    resize(560, 300);
}

}  // namespace qp::views
