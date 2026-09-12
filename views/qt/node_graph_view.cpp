/**
 * @file node_graph_view.cpp
 * @brief Implementation of the node editor canvas.
 */
#include "node_graph_view.hpp"
#include <qp/graph/ir/node_type_registry.hpp>

#include <qp/graph/mutate/command.hpp>

#include <QGraphicsItem>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPen>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace qp::views {
namespace {

// Node box geometry, in **graph** coordinates. Not pixels: the view applies the
// zoom, and a canvas that stored pixel sizes would forget where things were the
// moment the user zoomed or scrolled.
constexpr qreal kNodeWidth = 168.0;
constexpr qreal kNodeHeight = 62.0;
constexpr qreal kPortRadius = 4.0;
constexpr qreal kPortSpacing = 20.0;

/// @brief Keys a NodeId into the maps that hold its item and its position.
std::uint64_t key_of(qp::graph::NodeId id) noexcept {
    return (static_cast<std::uint64_t>(id.index) << 32) | static_cast<std::uint64_t>(id.generation);
}

/// @brief The nth default position, so a graph with no stored layout is still readable.
QPointF default_position(std::size_t index) noexcept {
    const auto column = static_cast<qreal>(index % 4U);
    const auto row = static_cast<qreal>(index / 4U);
    return QPointF(48.0 + column * (kNodeWidth + 48.0), 48.0 + row * (kNodeHeight + 68.0));
}

/// @brief Human-readable form of a NodeId.
QString describe(qp::graph::NodeId id) {
    return QStringLiteral("#%1.%2").arg(id.index).arg(id.generation);
}

/// @brief An error code as text, for the failure signal.
QString describe(qp::diag::ErrorCode code) {
    const std::string_view text = qp::diag::to_string(code);
    return QString::fromLatin1(text.data(), static_cast<int>(text.size()));
}

}  // namespace

// ===========================================================================
// Item types
// ===========================================================================

/**
 * @brief A node box in the canvas.
 *
 * It holds a node id and the strings it was built with, and nothing else about the
 * graph: no port list, no parameter values. Whatever it draws was resolved from
 * the catalog at rebuild time, so "the item and the graph agree" holds by
 * construction rather than by a synchronisation step somebody has to remember.
 */
class NodeItem final : public QGraphicsItem {
public:
    NodeItem(qp::graph::NodeId id, QString title, QString type_name, QStringList input_labels,
             QStringList output_labels, QPointF position)
        : id_(id),
          title_(std::move(title)),
          type_name_(std::move(type_name)),
          inputs_(std::move(input_labels)),
          outputs_(std::move(output_labels)) {
        setPos(position);
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setZValue(1.0);
        setToolTip(QStringLiteral("%1\n%2").arg(title_, type_name_));
    }

    [[nodiscard]] qp::graph::NodeId node_id() const noexcept { return id_; }

    void set_move_handler(std::function<void(NodeItem&)> handler) {
        on_settled_ = std::move(handler);
    }

    [[nodiscard]] QRectF boundingRect() const override {
        const qreal left = -kPortRadius;
        const qreal right = kNodeWidth + kPortRadius;
        const qreal extra = kPortRadius + 1.0;
        return QRectF(left, -extra, right - left, kNodeHeight + 2.0 * extra);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        const bool chosen = isSelected();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const QRectF box(0.0, 0.0, kNodeWidth, kNodeHeight);
        painter->setBrush(chosen ? QColor(0x2d, 0x5a, 0x88) : QColor(0x33, 0x37, 0x3d));
        painter->setPen(QPen(chosen ? QColor(0x6c, 0xb0, 0xf0) : QColor(0x55, 0x5b, 0x63), 1.5));
        painter->drawRoundedRect(box, 5.0, 5.0);

        painter->setPen(QColor(0xe6, 0xe8, 0xea));
        painter->drawText(QRectF(9.0, 4.0, kNodeWidth - 18.0, 20.0),
                          Qt::AlignLeft | Qt::AlignVCenter, title_);

        painter->setPen(QColor(0x8f, 0x97, 0xa1));
        painter->drawText(QRectF(9.0, 22.0, kNodeWidth - 18.0, 15.0),
                          Qt::AlignLeft | Qt::AlignVCenter, type_name_);

        // Input labels on the left, output labels on the right, each with a stub.
        // The labels come from the node's descriptor, so a node type whose ports
        // were registered shows them without the canvas knowing anything about it.
        painter->setPen(QColor(0xa8, 0xb0, 0xba));
        painter->setBrush(QColor(0x8a, 0xb4, 0xd8));
        const int rows = std::max(inputs_.size(), outputs_.size());
        for (int i = 0; i < rows; ++i) {
            const qreal y = 40.0 + i * kPortSpacing;
            if (i < inputs_.size()) {
                painter->drawEllipse(QPointF(0.0, y), kPortRadius, kPortRadius);
                painter->drawText(QRectF(8.0, y - 8.0, kNodeWidth / 2.0 - 10.0, 16.0),
                                  Qt::AlignLeft | Qt::AlignVCenter, inputs_.at(i));
            }
            if (i < outputs_.size()) {
                painter->drawEllipse(QPointF(kNodeWidth, y), kPortRadius, kPortRadius);
                painter->drawText(QRectF(kNodeWidth / 2.0, y - 8.0, kNodeWidth / 2.0 - 8.0, 16.0),
                                  Qt::AlignRight | Qt::AlignVCenter, outputs_.at(i));
            }
        }

        painter->setPen(QColor(0x69, 0x71, 0x7b));
        painter->drawText(QRectF(0.0, kNodeHeight - 13.0, kNodeWidth, 12.0),
                          Qt::AlignHCenter | Qt::AlignVCenter, describe(id_));
    }

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override {
        // Only a settled move is recorded. `ItemPositionChange` fires for every
        // intermediate candidate during a drag, and writing each one would push an
        // undo entry per mouse-move -- reversing one drag would take a hundred
        // undos. Positions are layout data anyway, not graph edits.
        if (change == ItemPositionChange && scene() != nullptr && on_settled_ != nullptr) {
            on_settled_(*this);
        }
        return QGraphicsItem::itemChange(change, value);
    }

private:
    qp::graph::NodeId id_{};
    QString title_{};
    QString type_name_{};
    QStringList inputs_{};
    QStringList outputs_{};
    std::function<void(NodeItem&)> on_settled_{};
};

/**
 * @brief An edge between two nodes, drawn as a straight line.
 *
 * Straight rather than curved on purpose: a curve needs the two endpoints' actual
 * port positions, and drawing a curve between box centres would look more
 * deliberate than the information warrants. A straight line is honest about what
 * this canvas currently knows.
 */
class EdgeItem final : public QGraphicsItem {
public:
    EdgeItem(QPointF from, QPointF to) : from_(from), to_(to) { setZValue(0.0); }

    void set_endpoints(QPointF from, QPointF to) {
        prepareGeometryChange();
        from_ = from;
        to_ = to;
        update();
    }

    [[nodiscard]] QRectF boundingRect() const override {
        return QRectF(from_, to_).normalized().adjusted(-2.0, -2.0, 2.0, 2.0);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(QColor(0x7a, 0x84, 0x90), 1.6));
        painter->drawLine(from_, to_);
    }

private:
    QPointF from_{};
    QPointF to_{};
};

// ===========================================================================
// Layout slot encoding
// ===========================================================================

namespace {

/**
 * @brief The layout slot is opaque bytes to the core, so the canvas picks the format.
 *
 * Chosen to be readable in a saved document rather than compact: a coordinate that
 * a human cannot read is a coordinate nobody can check, and this is the one part
 * of a document a user may reasonably want to fix by hand.
 *
 *     "3.1=120.0,80.0;7.1=340.0,80.0;"
 *
 * Lines rather than one long growing string, because appending to the previous
 * value (the obvious first attempt) makes each save quadratic in the number of
 * nodes and leaves stale entries for nodes that no longer exist.
 */
[[nodiscard]] std::map<std::uint64_t, QPointF> decode_layout(const std::string& slot) {
    std::map<std::uint64_t, QPointF> out;
    std::size_t at = 0;
    while (at < slot.size()) {
        const std::size_t line_end = slot.find(';', at);
        const std::string record = slot.substr(at, line_end == std::string::npos
                                                       ? std::string::npos
                                                       : line_end - at);
        const std::size_t equals = record.find('=');
        const std::size_t comma = record.find(',');
        if (equals != std::string::npos && comma != std::string::npos && comma > equals) {
            const std::size_t dot = record.find('.');
            if (dot != std::string::npos && dot < equals) {
                try {
                    const auto index =
                        static_cast<std::uint32_t>(std::stoul(record.substr(0, dot)));
                    const auto generation = static_cast<std::uint32_t>(
                        std::stoul(record.substr(dot + 1, equals - dot - 1)));
                    const double x = std::stod(record.substr(equals + 1, comma - equals - 1));
                    const double y = std::stod(record.substr(comma + 1));
                    out[key_of(qp::graph::NodeId{index, generation})] = QPointF(x, y);
                } catch (const std::exception&) {
                    // A malformed record is skipped, not fatal: the slot is
                    // user-visible data in a saved document, and a hand-edited
                    // typo must cost one node's position rather than the document.
                }
            }
        }
        if (line_end == std::string::npos) break;
        at = line_end + 1;
    }
    return out;
}

[[nodiscard]] std::string encode_layout(const std::map<std::uint64_t, QPointF>& positions) {
    std::ostringstream out;
    for (const auto& [key, position] : positions) {
        const auto index = static_cast<std::uint32_t>(key >> 32);
        const auto generation = static_cast<std::uint32_t>(key & 0xFFFFFFFFULL);
        out << index << '.' << generation << '=' << position.x() << ',' << position.y() << ';';
    }
    return out.str();
}

}  // namespace

// ===========================================================================
// The canvas
// ===========================================================================

/**
 * @brief The session listener that keeps the canvas in step.
 *
 * Nested so its lifetime is tied to the widget's: the session holds a raw pointer
 * to it, and a listener that outlived its view would be a dangling call on the
 * next change.
 */
class NodeGraphView::Bridge final : public qp::authoring::IChangeListener {
public:
    explicit Bridge(NodeGraphView& view) noexcept : view_(&view) {}

    void on_change(const qp::authoring::Change&) noexcept override {
        // Rebuilt from the session, never patched from the notification. The
        // notification says what changed; the graph is the authority on what is.
        view_->rebuild();
    }

private:
    NodeGraphView* view_;
};

NodeGraphView::NodeGraphView(qp::authoring::Session& session, const qp::graph::NodeTypeRegistry& catalog,
                             qp::authoring::Document& document, QWidget* parent)
    : QGraphicsView(parent), session_(session), catalog_(catalog), document_(document) {
    scene_ = new QGraphicsScene(this);
    setScene(scene_);
    setRenderHint(QPainter::Antialiasing, true);
    setDragMode(QGraphicsView::RubberBandDrag);
    setBackgroundBrush(QColor(0x1e, 0x21, 0x24));

    bridge_ = std::make_unique<Bridge>(*this);
    // Registered before the first rebuild, so a change landing between
    // construction and the first draw is not missed.
    (void)session_.add_listener(*bridge_);
    rebuild();
}

NodeGraphView::~NodeGraphView() = default;

void NodeGraphView::rebuild() {
    // The scene is cleared and rebuilt rather than patched. A partial update has
    // to know which commands affect which items, and getting that mapping subtly
    // wrong is how a canvas ends up drawing a node that no longer exists -- which
    // is the failure mode this whole design is arranged to prevent.
    scene_->clear();
    node_items_.clear();

    const qp::graph::Graph& graph = session_.graph();
    const std::map<std::uint64_t, QPointF> stored = decode_layout(document_.layouts().get(kGraphViewId));

    std::size_t index = 0;
    for (const qp::graph::NodeSlot& slot : graph.slots()) {
        if (!slot.node.id.valid()) continue;

        QString title = QString::fromStdString(slot.node.type_name);
        QStringList inputs;
        QStringList outputs;
        if (const qp::graph::NodeDesc* desc = catalog_.find(slot.node.type_name);
            desc != nullptr) {
            title = QString::fromStdString(desc->label.empty() ? desc->type_name : desc->label);
            // Only connectable ports are labelled as sockets. A parameter drawn
            // with a socket would invite a user to wire it, and the connection
            // would then be refused -- a UI that offers something the model
            // forbids is worse than one that omits it.
            for (const qp::graph::PortDesc& p : desc->inputs) {
                if (p.connectable) inputs << QString::fromStdString(
                    p.label.empty() ? p.name : p.label);
            }
            for (const qp::graph::PortDesc& p : desc->outputs) {
                if (p.connectable) outputs << QString::fromStdString(
                    p.label.empty() ? p.name : p.label);
            }
        }

        QPointF position = default_position(index);
        // A stored position wins; otherwise the deterministic grid. The fallback
        // is not the origin: every new node landing on (0, 0) would stack them,
        // and a canvas that stacks its nodes is unusable before it is wrong.
        if (const auto it = stored.find(key_of(slot.node.id)); it != stored.end()) {
            position = it->second;
        }

        auto* item = new NodeItem(slot.node.id, title, QString::fromStdString(slot.node.type_name),
                                  inputs, outputs, position);
        item->set_move_handler([this](NodeItem& moved) {
            remember_position(moved.node_id(), moved.pos());
        });
        scene_->addItem(item);
        node_items_.emplace(key_of(slot.node.id), item);
        ++index;
    }

    // Edges after nodes, so both endpoints exist. An edge whose endpoint is
    // missing is skipped rather than drawn to the origin: a line to nowhere reads
    // as a connection that exists.
    for (const qp::graph::Edge& edge : graph.edges()) {
        const auto from = node_items_.find(key_of(edge.from.node));
        const auto to = node_items_.find(key_of(edge.to.node));
        if (from == node_items_.end() || to == node_items_.end()) continue;
        auto* item = new EdgeItem(from->second->pos(), to->second->pos());
        scene_->addItem(item);
    }

    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-48.0, -48.0, 48.0, 48.0));

    // Frame what was just built, and do it by **fitting** rather than by centring.
    //
    // History, because the difference matters and the naive version looks right:
    //
    //   1. A `QGraphicsView` left at its default transform shows the scene's top-left corner,
    //      so nodes starting at (48, 48) appeared pinned to one side of the viewport.
    //   2. `centerOn` fixed that and introduced a worse problem. It scrolls a viewport over the
    //      scene at the current scale, so a graph **wider than the viewport is clipped wherever
    //      you centre it**. `default_position` lays nodes out in rows of four, 168 units apart,
    //      which is wider than this canvas on a 1280-wide window -- so the running shell showed
    //      one node of three while its own status line reported "nodes 3 | edges 2". The scene
    //      held all three; the view could not show them.
    //
    // `fitInView` with `KeepAspectRatio` shows the whole graph, scaling down only when it must.
    // It is also what makes the view's contents deterministic, which is what let a screenshot
    // turn "looks a bit off" into a finding rather than a matter of opinion.
    //
    // The `1.0` upper bound in the clamp: a graph of two nodes should not be blown up into two
    // enormous boxes, and `fitInView` will happily scale up forever to fill the viewport.
    //
    // The guard is `!framed_ && !empty`, not just `!framed_`: the constructor rebuilds once with
    // an empty graph, and setting the flag there consumed the one chance to frame. The result
    // was a view that never framed anything.
    if (!framed_ && !scene_->itemsBoundingRect().isEmpty()) {
        // Deferred to the next event-loop turn. The viewport has not been laid out yet during
        // the constructor -- its size is still the default -- and `fitInView` computes its scale
        // from that size, so fitting here would choose a scale for a viewport that does not
        // exist and never revisit it.
        pending_frame_ = true;
        QTimer::singleShot(0, this, [this] {
            if (!pending_frame_) return;
            pending_frame_ = false;
            framed_ = true;
            frame_graph();
        });
    }
}

void NodeGraphView::frame_graph() {
    const QRectF bounds = scene_->itemsBoundingRect();
    if (bounds.isEmpty() || viewport()->width() <= 0 || viewport()->height() <= 0) return;

    // Margin so the outermost nodes are not flush against the edge, which reads as clipped even
    // when it is not.
    const QRectF padded = bounds.adjusted(-24.0, -24.0, 24.0, 24.0);
    fitInView(padded, Qt::KeepAspectRatio);

    // Never magnify past 1:1. `fitInView` scales up to fill, so without this clamp a small graph
    // would be drawn larger than its authored size -- and a node editor that changes how large a
    // node is depending on how many nodes exist is a node editor whose layout cannot be
    // reasoned about.
    if (transform().m11() > 1.0) {
        resetTransform();
        centerOn(bounds.center());
    }
}

bool NodeGraphView::all_nodes_are_visible() const noexcept {
    // The visible area in scene coordinates. `mapToScene` on the viewport rect is the whole
    // computation: comparing against the viewport's *rect* would compare a scene-space position
    // with a widget-space rectangle, which is true for a view at the origin and false for every
    // scrolled view -- the one case that matters.
    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();

    for (const QGraphicsItem* item : scene_->items()) {
        if (dynamic_cast<const NodeItem*>(item) == nullptr) continue;
        const QRectF box = item->sceneBoundingRect();
        // `contains` rather than `intersects`: a node half off the edge is not visible in the
        // sense this reports. The defect this exists for showed a node whose right half was
        // present and whose left half, with its input port, was not.
        if (!visible.contains(box)) return false;
    }
    return true;
}

int NodeGraphView::node_item_count() const noexcept {
    int n = 0;
    for (const QGraphicsItem* item : scene_->items()) {
        if (dynamic_cast<const NodeItem*>(item) != nullptr) ++n;
    }
    return n;
}

int NodeGraphView::edge_item_count() const noexcept {
    int n = 0;
    for (const QGraphicsItem* item : scene_->items()) {
        if (dynamic_cast<const EdgeItem*>(item) != nullptr) ++n;
    }
    return n;
}

bool NodeGraphView::items_match_graph() const noexcept {
    return node_item_count() == static_cast<int>(session_.graph().node_count()) &&
           edge_item_count() == static_cast<int>(session_.graph().edge_count());
}

void NodeGraphView::remember_position(qp::graph::NodeId node, QPointF position) {
    // Written into the document's layout slot for this view id. The core never
    // reads it -- a coordinate is not a property of the graph -- and the whole
    // slot is re-encoded from the decoded map so that entries for deleted nodes
    // do not accumulate.
    std::map<std::uint64_t, QPointF> stored = decode_layout(document_.layouts().get(kGraphViewId));
    stored[key_of(node)] = position;
    document_.layouts().set(kGraphViewId, encode_layout(stored));
}

QPointF NodeGraphView::position_of(qp::graph::NodeId node) const {
    // Reads the slot directly through the same decoder the rebuild uses, rather
    // than going through remember_position: this function is const, and a second
    // decoder would be a second format.
    const std::map<std::uint64_t, QPointF> stored =
        decode_layout(document_.layouts().get(kGraphViewId));
    if (const auto it = stored.find(key_of(node)); it != stored.end()) return it->second;
    const std::size_t index = node.index == 0 ? 0 : static_cast<std::size_t>(node.index - 1);
    return default_position(index);
}

std::optional<qp::graph::NodeId> NodeGraphView::selected_node() const {
    for (QGraphicsItem* item : scene_->selectedItems()) {
        if (auto* node = dynamic_cast<NodeItem*>(item); node != nullptr) {
            return node->node_id();
        }
    }
    return std::nullopt;
}

}  // namespace qp::views
