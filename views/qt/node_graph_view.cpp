/**
 * @file node_graph_view.cpp
 * @brief Implementation of the node editor canvas.
 */
#include "node_graph_view.hpp"

#include "theme.hpp"

#include <qp/graph/ir/node_type_registry.hpp>

#include <qp/graph/mutate/command.hpp>

#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsSceneMouseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QScrollBar>
#include <QSizeF>
#include <QString>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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
//
// ## Why these numbers grew, and the rule they follow
//
// The first themed build drew 168x62 boxes with **Qt's default font**, which is 9 pt at 96 dpi -- roughly 12
// device pixels. A screenshot review called the labels "legible but small", and at a typical laptop's
// device-pixel ratio the port labels were around six physical pixels tall. Charter C5 asks for "large type",
// and the palette work had only made the *colours* a contract; the size was nobody's.
//
// So the sizes below are tied to an explicit point size rather than to a default. `kTitlePointSize` and
// `kLabelPointSize` are the authoritative numbers and the box is sized to hold them, not the other way round:
//
//   - the header must clear the title's line height plus its padding;
//   - the body must clear `kPortRows` rows of `kPortRowHeight`, the type name line and the footer.
//
// Qt reports font metrics in device pixels, so the box is computed from the **font actually resolved** --
// see `refresh_metrics()`. On a 144 dpi screen the same point size yields a taller line and the box grows with
// it, which is what "large type" has to mean on a machine that is not this one.
constexpr qreal kNodeWidth = 188.0;
constexpr qreal kPortRadius = 5.5;
/// Gap between the two columns of the fallback layout, in graph units.
constexpr qreal kColumnGap = 30.0;
/// Distance from the scene's edge to the first node. Also the margin `frame_graph` leaves when centring.
constexpr qreal kMargin = 28.0;
/// Point size of a node's title. Held in the same units Qt's font system uses so a high-DPI screen scales it.
constexpr int kTitlePointSize = 10;
/// Point size of every other label in a node box: the type name, the port names, the footer.
constexpr int kLabelPointSize = 9;
/// How many port rows a node box reserves room for. A node with more ports than this draws the first
/// `kPortRows` only -- see the note in `paint` for why that is the honest answer rather than a clipped box.
constexpr std::size_t kPortRows = 3;

namespace {

/// @brief The box metrics, measured from the fonts this platform actually resolved.
///
/// A struct rather than a pile of constants because every number below is derived: change a point size and the
/// box has to change with it, and a set of independent constants is a set that silently disagrees.
struct NodeMetrics final {
    qreal width = kNodeWidth;
    qreal header_height = 34.0;
    qreal height = 104.0;
    qreal port_row_height = 26.0;
    qreal first_port_y = 48.0;
};

/// @brief Measures the fonts and derives the box.
///
/// A free function rather than a member of `NodeMetrics`, and the reason is a compiler one that took a build to
/// find: calling `NodeMetrics::measure()` from `metrics()` below resolved to a *non-static* member on MSVC
/// (C2352) because the class and the caller sit in the same anonymous namespace. A free function has no such
/// ambiguity, and it keeps the struct a plain value.
///
/// @ownership   owns the returned value
/// @thread      ui
/// @pre         none
/// @post        Every field is consistent with the resolved fonts
/// @invariant   `height` grows with the fonts and never shrinks below the port block
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      Depends on the installed fonts and the screen's DPI
/// @frozen      no
/// @tests       qt.views.canvas.a_node_box_holds_its_type
[[nodiscard]] NodeMetrics measure_node_metrics() noexcept {
    QFont title;
    title.setPointSize(kTitlePointSize);
    title.setBold(true);
    QFont label;
    label.setPointSize(kLabelPointSize);

    const QFontMetricsF title_metrics{title};
    const QFontMetricsF label_metrics{label};

    NodeMetrics m;
    // Padding above and below the title: a header exactly one line tall reads as clipped, and a title touching
    // the category band's edge reads as a rendering bug.
    m.header_height = title_metrics.height() + 14.0;
    m.port_row_height = label_metrics.height() + 8.0;
    // The body: the type-name line, the port block, and the footer.
    m.height = m.header_height + label_metrics.height() + 6.0 +
               static_cast<qreal>(kPortRows) * m.port_row_height + 6.0 + label_metrics.height() + 4.0;
    m.first_port_y = m.header_height + label_metrics.height() + 6.0 + m.port_row_height / 2.0;
    return m;
}

/// @brief The metrics, measured once per process.
///
/// A function-local constant rather than a namespace-scope one: a `QFont` at namespace scope would be
/// constructed before `QApplication` exists, which Qt warns about and which is undefined enough to avoid.
[[nodiscard]] const NodeMetrics& metrics() noexcept {
    static const NodeMetrics measured = measure_node_metrics();
    return measured;
}

}  // namespace

/// @brief Keys a NodeId into the maps that hold its item and its position.
std::uint64_t key_of(qp::graph::NodeId id) noexcept {
    return (static_cast<std::uint64_t>(id.index) << 32) | static_cast<std::uint64_t>(id.generation);
}

/// @brief The nth default position, so a graph with no stored layout is still readable.
///
/// ## Two columns, not four
///
/// The first version laid nodes out four to a row, which is wider than the canvas on an ordinary window -- so
/// every fresh graph was scaled down to fit, and the scale-down is exactly what undid the legible type size
/// above. A fallback layout that forces the view to shrink is a fallback layout that loses the argument it
/// exists for.
///
/// Two columns is the largest row count that fits a default canvas at **1:1**, so the common case (a handful of
/// nodes, nothing stored yet) is drawn at its authored size and nothing is clipped. A graph that grows past
/// four nodes scrolls, which is what a node editor does.
///
/// Column order fills left-to-right then down, so node 1 and node 2 -- the usual source and model -- end up
/// side by side with a wire between them.
QPointF default_position(std::size_t index) noexcept {
    constexpr std::size_t kColumns = 2;
    const auto column = static_cast<qreal>(index % kColumns);
    const auto row = static_cast<qreal>(index / kColumns);
    return QPointF(kMargin + column * (kNodeWidth + kColumnGap),
                   kMargin + row * (metrics().height + 48.0));
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
 * It holds a node id, the strings it was built with, and its **port numbers**, and nothing else about the graph:
 * no parameter values, no edge list. Whatever it draws was resolved from the catalog at rebuild time, so "the
 * item and the graph agree" holds by construction rather than by a synchronisation step somebody has to
 * remember.
 *
 * ## Why the port numbers are here now
 *
 * They were not, and an edge was therefore drawn between two box **centres** -- so a connection existed in the
 * graph and the canvas showed a line from the middle of one box to the middle of another, touching neither of
 * the ports it actually joined. The comment that used to sit on `EdgeItem` called that "honest about what this
 * canvas currently knows"; it was honest about a gap that did not need to exist. A port number is one integer
 * and the row it is drawn at is already computed in `paint`, so the canvas knew enough all along.
 *
 * Which is why `port_scene_pos` exists below and why the port labels are no longer the only thing kept: a
 * connection is `(node, port number)`, and a canvas that stores only the label cannot say where the line goes.
 */
class NodeItem final : public QGraphicsItem {
public:
    /// @brief One port as the canvas draws it: the number the graph addresses it by, and its label.
    struct Port final {
        qp::graph::PortIndex number = 0;
        QString label{};
    };

    NodeItem(qp::graph::NodeId id, QString title, QString type_name, QString category,
             std::vector<Port> inputs, std::vector<Port> outputs, QPointF position)
        : id_(id),
          title_(std::move(title)),
          type_name_(std::move(type_name)),
          category_(std::move(category)),
          inputs_(std::move(inputs)),
          outputs_(std::move(outputs)) {
        setPos(position);
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setZValue(1.0);
        setToolTip(QStringLiteral("%1\n%2").arg(title_, type_name_));
    }

    [[nodiscard]] qp::graph::NodeId node_id() const noexcept { return id_; }

    [[nodiscard]] const std::vector<Port>& inputs() const noexcept { return inputs_; }
    [[nodiscard]] const std::vector<Port>& outputs() const noexcept { return outputs_; }

    /**
     * @brief Where the centre of `port`'s stub is, in **scene** coordinates.
     *
     * The row arithmetic is the same expression `paint` uses, and the duplication is the risk: two places that
     * decide where a stub is can disagree, and the symptom would be a line ending near a port rather than on it.
     * `qt.views.nodegraph.an_edge_ends_on_its_ports` asserts that they agree by measuring a rendered edge
     * against the stub the paint code drew.
     *
     * @param port         The port number the graph addresses it by.
     * @param is_output    Which side. Inputs are on the left edge of the box, outputs on the right.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        A point on the box's edge, or a point outside it for a port the box does not draw
     * @invariant   Depends only on this item's position and the metrics
     * @errors      noexcept
     * @complexity  O(ports on that side)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.nodegraph.an_edge_ends_on_its_ports
     */
    [[nodiscard]] QPointF port_scene_pos(qp::graph::PortIndex port, bool is_output) const noexcept {
        const qreal y = port_local_y(port, is_output);
        if (std::isnan(y)) return mapToScene(QPointF{0.0, 0.0});
        const qreal x = is_output ? metrics().width : 0.0;
        return mapToScene(QPointF{x, y});
    }

    /**
     * @brief The port nearest `scene_point` within `radius`, or nothing.
     *
     * The hit test a hand-drawn connection needs. `radius` is in **scene** units and a caller should scale it by
     * the view's zoom, or a zoomed-out canvas would make ports impossible to grab -- which is why the radius is a
     * parameter rather than a constant here.
     *
     * A port the box does not draw is never returned: a connection to an invisible port would be an edit the
     * user cannot see, and could not undo by aiming at it again.
     *
     * @param scene_point Where the user is pointing, in scene coordinates.
     * @param radius      How far away still counts, in scene units.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        The nearest drawn port within `radius`, or nothing
     * @invariant   Never returns a port outside `kPortRows`
     * @errors      noexcept
     * @complexity  O(ports)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.nodegraph.a_dragged_connection_joins_two_ports
     */
    [[nodiscard]] std::optional<std::pair<qp::graph::PortIndex, bool>>
    nearest_port(QPointF scene_point, qreal radius) const noexcept {
        const Port* best = nullptr;
        bool best_is_output = false;
        qreal best_distance = radius;
        for (const bool is_output : {false, true}) {
            const std::vector<Port>& side = is_output ? outputs_ : inputs_;
            for (std::size_t i = 0; i < side.size() && i < kPortRows; ++i) {
                const qreal y = port_local_y(side[i].number, is_output);
                if (std::isnan(y)) continue;
                const QPointF stub = mapToScene(QPointF{is_output ? metrics().width : 0.0, y});
                const QPointF delta = stub - scene_point;
                const qreal distance = std::hypot(delta.x(), delta.y());
                if (distance <= best_distance) {
                    best_distance = distance;
                    best = &side[i];
                    best_is_output = is_output;
                }
            }
        }
        if (best == nullptr) return std::nullopt;
        return std::make_pair(best->number, best_is_output);
    }

    void set_move_handler(std::function<void(NodeItem&)> handler) {
        on_settled_ = std::move(handler);
    }

    /// @brief Called while the item is being dragged, so the edges attached to it can follow immediately.
    void set_drag_handler(std::function<void(NodeItem&)> handler) {
        on_dragged_ = std::move(handler);
    }

    [[nodiscard]] QRectF boundingRect() const override {
        const NodeMetrics& m = metrics();
        const qreal left = -kPortRadius;
        const qreal right = m.width + kPortRadius;
        const qreal extra = kPortRadius + 1.0;
        return QRectF(left, -extra, right - left, m.height + 2.0 * extra);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        const bool chosen = isSelected();
        painter->setRenderHint(QPainter::Antialiasing, true);

        // Every colour from the palette. The literals that used to be here were a palette with no definition:
        // "is the node title readable" was answerable only by looking, and `theme.text_clears_wcag_aa` could
        // not see this file at all. The category tint is the node's own group colour, so a type registered by a
        // plugin is drawn in its category's colour without the canvas knowing the category exists.
        //
        // ## Why the box has two zones
        //
        // The first themed build tinted the whole box and wrote every label inside it. A screenshot review found
        // the secondary line washed out on one group, and making the pair a checked property
        // (`theme.text_on_a_tinted_node_is_readable`) then showed the deeper problem: on `#8C663F` the best
        // available ink measures 4.18:1, so **no** colour in this palette can label that fill readably. A
        // mid-tone fill is simply not a background for body text.
        //
        // So the colour identifies the node and the text sits on a surface: a header band in the category colour
        // carrying the title (one short line, at the best ink the fill admits), and a body on `surface_raised`
        // carrying everything that has to be read -- the type name, the port labels, the footer. The pairs the
        // body uses are the ones `theme.text_clears_wcag_aa` already asserts.
        const qt::theme::Palette& c = qt::theme::palette();
        const NodeMetrics& m = metrics();
        const QRectF box(0.0, 0.0, m.width, m.height);
        const qreal header_height = m.header_height;

        QColor band = qt::theme::category_colour(category_);
        if (chosen) {
            band = band.lighter(115);
        }
        const auto as_theme_rgb = [](const QColor& colour) {
            return qt::theme::Rgb{static_cast<std::uint8_t>(colour.red()),
                                  static_cast<std::uint8_t>(colour.green()),
                                  static_cast<std::uint8_t>(colour.blue())};
        };

        // The fonts are **explicit**, not inherited, and that is the fix for the finding this code was changed
        // for: a node box drawn at Qt's default font is 9 pt, which at a laptop's device-pixel ratio puts a port
        // label at about six physical pixels. C5 asks for large type; a default is not a decision.
        QFont title_font = painter->font();
        title_font.setPointSize(kTitlePointSize);
        title_font.setBold(true);
        QFont label_font = painter->font();
        label_font.setPointSize(kLabelPointSize);

        painter->setPen(Qt::NoPen);
        painter->setBrush(band);
        painter->drawRoundedRect(box, 5.0, 5.0);
        painter->setBrush(qt::theme::to_qcolor(c.surface_raised));
        painter->drawRect(QRectF(0.0, header_height, m.width, m.height - header_height));

        // One outline around the whole box, so the category colour reads as a header rather than as a fill that
        // failed to cover the bottom.
        //
        // The selected ring is derived from the **band's own ink** rather than from `accent`, and that is a
        // measurement rather than a preference: `accent` against the selected output band measures 1.8:1, which
        // is a selection state a user cannot see. The ink chosen for the title is by construction one of the two
        // most contrasting colours available for that fill, so a lighter step of it is guaranteed to read.
        const QColor title_ink = qt::theme::text_on(as_theme_rgb(band));
        const QColor ring = chosen ? title_ink.lighter(150) : qt::theme::to_qcolor(c.edge_dim);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(ring, chosen ? 2.0 : 1.5));
        painter->drawRoundedRect(box, 5.0, 5.0);

        painter->setFont(title_font);
        painter->setPen(title_ink);
        painter->drawText(QRectF(11.0, 2.0, m.width - 22.0, header_height - 4.0),
                          Qt::AlignLeft | Qt::AlignVCenter, title_);

        painter->setFont(label_font);
        painter->setPen(qt::theme::to_qcolor(c.text_muted));
        painter->drawText(QRectF(11.0, header_height + 1.0, m.width - 22.0, m.port_row_height),
                          Qt::AlignLeft | Qt::AlignVCenter, type_name_);

        // Input labels on the left, output labels on the right, each with a stub.
        // The labels come from the node's descriptor, so a node type whose ports
        // were registered shows them without the canvas knowing anything about it.
        //
        // `kPortRows` is a **ceiling**, and reaching it draws no further rows rather than a growing box. A box
        // that grew with its port count would make a node's size depend on a plugin's descriptor -- so two nodes
        // of the same type would differ, and the layout algorithm would have nothing stable to work with. A node
        // with more than three connectable ports on one side therefore shows three; the descriptor panel lists
        // all of them, and the count is visible in the palette. That is a real limitation and it is stated
        // rather than hidden by a taller box.
        //
        // The row's y comes from `port_local_y`, which is also what `port_scene_pos` uses, so the stub a user
        // sees and the point an edge is drawn to are the same arithmetic rather than two implementations of it.
        painter->setPen(Qt::NoPen);
        const std::size_t rows = std::min(std::max(inputs_.size(), outputs_.size()), kPortRows);
        for (std::size_t i = 0; i < rows; ++i) {
            if (i < inputs_.size()) {
                const qreal y = port_local_y(inputs_[i].number, /*is_output=*/false);
                // An input is drawn as a **ring**: a connection arrives here, and an open circle reads as a
                // socket. An output is a filled disc, so the direction of a wire is visible without tracing it.
                painter->setBrush(qt::theme::to_qcolor(c.surface_raised));
                painter->setPen(QPen(qt::theme::to_qcolor(c.accent), 1.6));
                painter->drawEllipse(QPointF(0.0, y), kPortRadius, kPortRadius);
                painter->setPen(qt::theme::to_qcolor(c.text));
                painter->drawText(QRectF(10.0, y - m.port_row_height / 2.0, m.width / 2.0 - 12.0,
                                         m.port_row_height),
                                  Qt::AlignLeft | Qt::AlignVCenter, inputs_[i].label);
            }
            if (i < outputs_.size()) {
                const qreal y = port_local_y(outputs_[i].number, /*is_output=*/true);
                painter->setPen(Qt::NoPen);
                painter->setBrush(qt::theme::to_qcolor(c.accent));
                painter->drawEllipse(QPointF(m.width, y), kPortRadius, kPortRadius);
                painter->setPen(qt::theme::to_qcolor(c.text));
                painter->drawText(QRectF(m.width / 2.0, y - m.port_row_height / 2.0,
                                         m.width / 2.0 - 10.0, m.port_row_height),
                                  Qt::AlignRight | Qt::AlignVCenter, outputs_[i].label);
            }
        }

        // `text_muted`, not `text_disabled`: this is a node's own identity, meant to be read. The dark palette's
        // disabled grey measures 2.5:1 on `surface_raised`, which is below the graphics floor -- and a disabled
        // colour on information that is not disabled is how a palette's own rules get quietly broken.
        painter->setPen(qt::theme::to_qcolor(c.text_muted));
        painter->drawText(QRectF(0.0, m.height - m.port_row_height, m.width, m.port_row_height - 2.0),
                          Qt::AlignHCenter | Qt::AlignVCenter, describe(id_));
    }

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override {
        // Only a settled move is recorded. `ItemPositionChange` fires for every
        // intermediate candidate during a drag, and writing each one would push an
        // undo entry per mouse-move -- reversing one drag would take a hundred
        // undos. Positions are layout data anyway, not graph edits.
        if (change == ItemPositionChange && scene() != nullptr) {
            // Two different jobs, and both are needed while the mouse is moving: the edges attached to this node
            // must follow it **during** the drag, and the position must be remembered **once** at the end.
            //
            // The first used to be missing entirely, which is why a node could be dragged away from its own
            // connections: the edge kept the endpoints it was built with, so the line stayed where the node had
            // been and the graph looked disconnected while the graph said otherwise.
            if (on_dragged_ != nullptr) on_dragged_(*this);
            if (on_settled_ != nullptr) on_settled_(*this);
        }
        return QGraphicsItem::itemChange(change, value);
    }

private:
    /// @brief The y of `port`'s row in item coordinates, or NaN when the box does not draw that port.
    ///
    /// The single expression `paint` and `port_scene_pos` both use. `paint` calls it too, so there is one place
    /// that decides where a row is rather than two that must agree.
    [[nodiscard]] qreal port_local_y(qp::graph::PortIndex port, bool is_output) const noexcept {
        const std::vector<Port>& side = is_output ? outputs_ : inputs_;
        const NodeMetrics& m = metrics();
        for (std::size_t i = 0; i < side.size() && i < kPortRows; ++i) {
            if (side[i].number == port) {
                return m.first_port_y + static_cast<qreal>(i) * m.port_row_height;
            }
        }
        return std::numeric_limits<qreal>::quiet_NaN();
    }

    qp::graph::NodeId id_{};
    QString title_{};
    QString type_name_{};
    /// The descriptor's `category`, kept so the box can be tinted by group. Read from the catalog at rebuild
    /// time like every other string here -- the item holds no view of the graph of its own.
    QString category_{};
    std::vector<Port> inputs_{};
    std::vector<Port> outputs_{};
    std::function<void(NodeItem&)> on_settled_{};
    std::function<void(NodeItem&)> on_dragged_{};
};

/**
 * @brief An edge between two nodes, drawn as a straight line.
 *
 * Straight rather than curved on purpose: a curve needs the two endpoints' actual
 * port positions, and drawing a curve between box centres would look more
 * deliberate than the information warrants. A straight line is honest about what
 * this canvas currently knows.
 */
/**
 * @brief An edge between two nodes, drawn from one port stub to the other.
 *
 * ## It knows which ports it joins, and that is the point
 *
 * The first version stored a pair of points taken from two box **centres** at rebuild time, and the comment
 * above it argued that a straight line was "honest about what this canvas currently knows". It was honest about
 * a gap that did not need to exist: the endpoint of a connection is `(node, port number)`, the port number was
 * in the graph all along, and the row a port is drawn at was already computed in `NodeItem::paint`. The result
 * of not knowing was a line that touched neither port, so a **correct** graph looked like a broken one.
 *
 * So this item holds the two `PortRef`s and the two items, and derives its endpoints on demand. That is also
 * what makes a node drag work: `refresh` recomputes both ends from wherever the nodes are now, so the line
 * follows the box instead of staying where the box used to be.
 *
 * Straight rather than curved: with the endpoints on the stubs, a curve would add nothing a reader needs, and
 * a straight segment between two points is the one shape whose geometry cannot be subtly wrong.
 */
class EdgeItem final : public QGraphicsItem {
public:
    EdgeItem(NodeItem& from, NodeItem& to, qp::graph::PortRef from_port, qp::graph::PortRef to_port)
        : from_item_(&from), to_item_(&to), from_port_(from_port), to_port_(to_port) {
        setZValue(0.0);
        refresh();
    }

    /// @brief The output port this edge leaves, so a caller can tell whether a node drag concerns it.
    [[nodiscard]] const qp::graph::PortRef& from_port() const noexcept { return from_port_; }
    /// @brief The input port this edge reaches.
    [[nodiscard]] const qp::graph::PortRef& to_port() const noexcept { return to_port_; }

    /// @brief Recomputes both endpoints from where its two nodes are **now**.
    void refresh() {
        prepareGeometryChange();
        from_ = from_item_->port_scene_pos(from_port_.port, /*is_output=*/true);
        to_ = to_item_->port_scene_pos(to_port_.port, /*is_output=*/false);
        update();
    }

    [[nodiscard]] QPointF from_point() const noexcept { return from_; }
    [[nodiscard]] QPointF to_point() const noexcept { return to_; }

    [[nodiscard]] QRectF boundingRect() const override {
        // Two pixels of slack so the pen's width is inside the update region; without it a repaint can leave a
        // hairline of the old line behind on a diagonal.
        return QRectF(from_, to_).normalized().adjusted(-2.0, -2.0, 2.0, 2.0);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        const qt::theme::Palette& c = qt::theme::palette();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(qt::theme::to_qcolor(c.edge), 1.6));
        painter->drawLine(from_, to_);
        // A small dot at the source end, so two edges crossing between the same pair of boxes can still be told
        // apart by which stub each one starts at. Without it the line's direction is only inferable from the
        // ring/disc difference between the ports themselves.
        painter->setPen(Qt::NoPen);
        painter->setBrush(qt::theme::to_qcolor(c.edge));
        painter->drawEllipse(from_, 2.0, 2.0);
    }

private:
    NodeItem* from_item_ = nullptr;
    NodeItem* to_item_ = nullptr;
    qp::graph::PortRef from_port_{};
    qp::graph::PortRef to_port_{};
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
    setBackgroundBrush(qt::theme::to_qcolor(qt::theme::palette().surface));
    // The canvas takes focus on a click, which is what makes the Delete key reach `keyPressEvent` rather than the
    // palette list. `StrongFocus` rather than `ClickFocus`: a canvas a user has tabbed to should also respond.
    setFocusPolicy(Qt::StrongFocus);
    // The scene rect is the scrollable area, and it is set from the graph plus a margin in `frame_graph`. Without
    // this, a graph dragged past its own bounds would scroll into empty space with no limit -- a canvas a user can
    // lose their graph in.
    setSceneRect(scene_->sceneRect());

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
    // The scene owned them, so they are gone with it; holding the pointers would be holding dangling ones.
    edge_items_.clear();
    drag_from_ = nullptr;
    drag_line_ = nullptr;

    const qp::graph::Graph& graph = session_.graph();
    const std::map<std::uint64_t, QPointF> stored = decode_layout(document_.layouts().get(kGraphViewId));

    std::size_t index = 0;
    for (const qp::graph::NodeSlot& slot : graph.slots()) {
        if (!slot.node.id.valid()) continue;

        QString title = QString::fromStdString(slot.node.type_name);
        QString category{};
        std::vector<NodeItem::Port> inputs;
        std::vector<NodeItem::Port> outputs;
        if (const qp::graph::NodeDesc* desc = catalog_.find(slot.node.type_name);
            desc != nullptr) {
            title = QString::fromStdString(desc->label.empty() ? desc->type_name : desc->label);
            // The group colour the box is tinted with, so a palette that groups by category and a canvas that
            // colours by category cannot disagree about which category a node is in.
            category = QString::fromStdString(desc->category);
            // Only connectable ports are drawn as sockets. A parameter drawn with a socket would invite a user
            // to wire it, and the connection would then be refused -- a UI that offers what the model forbids is
            // worse than one that omits it.
            //
            // The **port number** travels with the label, because a connection is `(node, port number)` and a
            // canvas holding only labels cannot say which port an edge belongs to. That omission is the whole
            // reason edges used to be drawn between box centres.
            for (const qp::graph::PortDesc& p : desc->inputs) {
                if (!p.connectable) continue;
                inputs.push_back(NodeItem::Port{
                    p.number, QString::fromStdString(p.label.empty() ? p.name : p.label)});
            }
            for (const qp::graph::PortDesc& p : desc->outputs) {
                if (!p.connectable) continue;
                outputs.push_back(NodeItem::Port{
                    p.number, QString::fromStdString(p.label.empty() ? p.name : p.label)});
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
                                  category, std::move(inputs), std::move(outputs), position);
        item->set_move_handler([this](NodeItem& moved) {
            remember_position(moved.node_id(), moved.pos());
        });
        // During the drag, not only at the end: an edge whose endpoints are only refreshed on the next rebuild
        // stays where the node used to be for the whole gesture.
        item->set_drag_handler([this](NodeItem& moved) { update_edges_for(moved); });
        scene_->addItem(item);
        node_items_.emplace(key_of(slot.node.id), item);
        ++index;
    }

    // Edges after nodes, so both endpoints exist. An edge whose endpoint is
    // missing is skipped rather than drawn to the origin: a line to nowhere reads
    // as a connection that exists.
    //
    // Each line runs from the **output stub** it leaves to the **input stub** it reaches, both taken from the
    // items. Drawing centre to centre is what this replaced: the graph had an edge, the canvas had a line, and
    // the line touched neither port -- so a correct graph looked like a wrong one, which is the worst of the two
    // available failures because the user's next move is to re-draw a connection that already exists.
    for (const qp::graph::Edge& edge : graph.edges()) {
        const auto from = node_items_.find(key_of(edge.from.node));
        const auto to = node_items_.find(key_of(edge.to.node));
        if (from == node_items_.end() || to == node_items_.end()) continue;
        auto* item = new EdgeItem(*from->second, *to->second, edge.from, edge.to);
        scene_->addItem(item);
        edge_items_.push_back(item);
    }

    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-kMargin, -kMargin, kMargin, kMargin));

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

void NodeGraphView::update_edges_for(const NodeItem& moved) {
    // Every edge touching this node, and only those. A linear scan is right here: a canvas holds tens of nodes
    // and a drag emits one of these per mouse-move, so an index would be more code than it saves -- and an index
    // is one more thing that can disagree with the graph after an edit.
    for (EdgeItem* edge : edge_items_) {
        if (edge->from_port().node == moved.node_id() || edge->to_port().node == moved.node_id()) {
            edge->refresh();
        }
    }
}

std::pair<QPointF, QPointF> NodeGraphView::edge_endpoints(std::size_t index) const noexcept {
    if (index >= edge_items_.size()) return {QPointF{}, QPointF{}};
    const EdgeItem* edge = edge_items_[index];
    return {edge->from_point(), edge->to_point()};
}

bool NodeGraphView::connect_ports(qp::graph::NodeId from_node, qp::graph::PortIndex from_port,
                                  qp::graph::NodeId to_node, qp::graph::PortIndex to_port) {
    // The command goes through the **session**, like every other edit this canvas makes. A canvas that wrote to
    // the graph directly would produce a change the undo stack never saw and the other panels were never told
    // about -- and the resulting repair, a rebuild from a notification that never fired, would be a redraw of
    // what the graph used to be.
    qp::graph::Connect command;
    command.from = qp::graph::PortRef{from_node, from_port, qp::graph::PortDirection::output};
    command.to = qp::graph::PortRef{to_node, to_port, qp::graph::PortDirection::input};

    const qp::diag::Result<void> applied = session_.apply(command);
    if (!applied.has_value()) {
        // Reported, not swallowed: a refused connection is the user's edit being declined (a type mismatch, an
        // input already fed, a cycle) and the sentence is the only place that reason appears.
        //
        // `Q_EMIT`, not `emit`: this target sets `QT_NO_KEYWORDS`, which turns the macro off so a core header may
        // use `emit` or `signals` as an identifier. The explicit spelling is the one that survives it.
        Q_EMIT mutation_failed(describe(applied.error()));
        return false;
    }
    // No rebuild here: the session tells its listeners, and this canvas is one of them. Rebuilding as well would
    // draw the scene twice for one edit, and the second draw is the one that could disagree.
    return true;
}

void NodeGraphView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        // Middle-drag pans. `QGraphicsView::ScrollHandDrag` would be the built-in answer and it is not available:
        // it drags with the **left** button, and the left button is already carrying three gestures (rubber-band
        // select, move a node, draw a connection). A pan that costs a gesture would cost one of those.
        last_pan_pos_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || scene_ == nullptr) {
        QGraphicsView::mousePressEvent(event);
        return;
    }

    const QPointF scene_point = mapToScene(event->pos());
    // A grab radius in **device** pixels, converted to scene units, so the gesture feels the same at every zoom.
    const qreal zoom = transform().m11() > 0.0 ? transform().m11() : 1.0;
    const qreal radius = kPortGrabPixels / zoom;

    for (const auto& [key, item] : node_items_) {
        (void)key;
        // Outputs only. A wire is drawn from where a value leaves to where it arrives, and starting from an
        // input would need a second gesture that means the reverse -- two gestures for one relation is how a
        // user ends up unsure which one they performed.
        const auto found = item->nearest_port(scene_point, radius);
        if (!found.has_value() || !found->second) continue;

        drag_from_ = item;
        drag_from_port_ = found->first;
        const QPointF start = item->port_scene_pos(found->first, /*is_output=*/true);

        // The preview: a plain line in the edge colour, owned by the scene and destroyed when the gesture ends.
        // A dashed one would be prettier and would also have to be re-styled on every theme change.
        drag_line_ = scene_->addLine(QLineF(start, scene_point),
                                     QPen(qt::theme::to_qcolor(qt::theme::palette().accent), 1.6));
        drag_line_->setZValue(2.0);
        event->accept();
        return;
    }

    // Not on a port: the base class handles it, which is what makes dragging a node by its body still work.
    QGraphicsView::mousePressEvent(event);
}

void NodeGraphView::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::MiddleButton) {
        // Pan by the delta in **device** pixels, applied to the scrollbars. `QGraphicsView::ScrollHandDrag` would
        // be the built-in answer and it is not available: it drags with the **left** button, and the left button is
        // already carrying three gestures (rubber-band select, move a node, draw a connection). A pan that costs a
        // gesture would cost one of those.
        const QPoint delta = event->pos() - last_pan_pos_;
        last_pan_pos_ = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        event->accept();
        return;
    }
    if (drag_from_ == nullptr || drag_line_ == nullptr) {
        QGraphicsView::mouseMoveEvent(event);
        return;
    }
    drag_line_->setLine(QLineF(drag_from_->port_scene_pos(drag_from_port_, /*is_output=*/true),
                               mapToScene(event->pos())));
    event->accept();
}

void NodeGraphView::wheelEvent(QWheelEvent* event) {
    // The wheel **zooms**, and that is a decision rather than the default: a node graph is a diagram, not a
    // document, and a user reaching for the wheel over a diagram expects to scale it. Panning stays on the middle
    // button, the scrollbars and the arrow keys, so nothing is lost by re-purposing the wheel.
    const int steps = event->angleDelta().y();
    if (steps == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    zoom_by(steps > 0 ? 1.1 : 1.0 / 1.1);
    event->accept();
}

void NodeGraphView::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (delete_selection()) {
                event->accept();
                return;
            }
            break;
        case Qt::Key_F:
            // The zoom-to-fit a user reaches for after scrolling away. The same call `rebuild` makes, so there is
            // one framing rule rather than a shortcut that frames differently from the initial view.
            frame_graph();
            event->accept();
            return;
        default:
            break;
    }
    QGraphicsView::keyPressEvent(event);
}

void NodeGraphView::zoom_by(qreal factor) {
    const qreal current = transform().m11();
    const qreal clamped = std::clamp(current * factor, kMinimumScale, kMaximumScale);
    if (std::abs(clamped - current) < 1e-9) return;  // Already at the limit; do not jitter.

    // Anchored on the **viewport centre** rather than on the cursor. Cursor-anchored zoom is right for a map and
    // wrong for a diagram: a user has usually selected something in the middle and is about to drag it, and a zoom
    // that slides that node out from under the cursor makes their next click land somewhere else.
    const QPointF centre = viewport_centre_in_scene();
    resetTransform();
    scale(clamped, clamped);
    centerOn(centre);
}

QPointF NodeGraphView::viewport_centre_in_scene() const {
    return mapToScene(viewport()->rect().center());
}

void NodeGraphView::reveal(qp::graph::NodeId node) {
    const auto it = node_items_.find(key_of(node));
    if (it == node_items_.end()) return;
    // A quarter-viewport of margin, so the node lands inside the view with room to see what it connects to rather
    // than against the edge. `ensureVisible` rather than `centerOn`: centring moves the whole picture, and a
    // canvas that jumps whenever something is added is one where a user loses their place.
    ensureVisible(it->second->sceneBoundingRect(), std::max(24, viewport()->width() / 4),
                  std::max(24, viewport()->height() / 4));
}

bool NodeGraphView::delete_selection() {
    const std::optional<qp::graph::NodeId> chosen = selected_node();
    if (!chosen.has_value()) return false;

    // The session decides what removing a node means -- including the edges that touched it, which the graph
    // layer drops with it. A canvas that also issued a `Disconnect` per edge would be duplicating a rule it does
    // not own, and the duplication would be wrong the first time the rule changed.
    qp::graph::RemoveNode command;
    command.id = *chosen;
    const qp::diag::Result<void> applied = session_.apply(command);
    if (!applied.has_value()) {
        Q_EMIT mutation_failed(describe(applied.error()));
        return false;
    }
    return true;
}

bool NodeGraphView::disconnect_selection() {
    const std::optional<qp::graph::NodeId> chosen = selected_node();
    if (!chosen.has_value()) return false;

    // The first **fed** input. "Remove the edge" is ambiguous when a node has several, and the choice has to be
    // one a user can predict: the topmost input with a wire is the one they are looking at when they aim at the
    // node, and the graph's edges come back in insertion order.
    for (const qp::graph::Edge& edge : session_.graph().edges()) {
        if (edge.to.node != *chosen) continue;

        qp::graph::Disconnect command;
        command.input = edge.to;
        const qp::diag::Result<void> applied = session_.apply(command);
        if (!applied.has_value()) {
            Q_EMIT mutation_failed(describe(applied.error()));
            return false;
        }
        return true;
    }
    return false;
}

void NodeGraphView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        // The pan ends with the button that started it. `ScrollHandDrag` cannot be used for this because it takes
        // over the left button, which is already spoken for by selection, node dragging and drawing connections --
        // so a hand-rolled middle-button pan is the option that does not cost a gesture.
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }
    if (drag_from_ == nullptr || drag_line_ == nullptr) {
        QGraphicsView::mouseReleaseEvent(event);
        return;
    }

    const QPointF scene_point = mapToScene(event->pos());
    const qreal zoom = transform().m11() > 0.0 ? transform().m11() : 1.0;
    const qreal radius = kPortGrabPixels / zoom;

    NodeItem* target = nullptr;
    qp::graph::PortIndex target_port = qp::graph::kNoPort;
    for (const auto& [key, item] : node_items_) {
        (void)key;
        const auto found = item->nearest_port(scene_point, radius);
        // Inputs only, and never the node the wire started from: a node feeding itself is a cycle, and the
        // command bus would refuse it -- reporting that refusal for a gesture the user made at their own source
        // node would name a rule they did not break.
        if (!found.has_value() || found->second || item == drag_from_) continue;
        target = item;
        target_port = found->first;
        break;
    }

    NodeItem* const source = drag_from_;
    const qp::graph::PortIndex source_port = drag_from_port_;
    scene_->removeItem(drag_line_);
    delete drag_line_;
    drag_line_ = nullptr;
    drag_from_ = nullptr;
    drag_from_port_ = qp::graph::kNoPort;

    if (target != nullptr && target_port != qp::graph::kNoPort) {
        (void)connect_ports(source->node_id(), source_port, target->node_id(), target_port);
    }
    // A release that landed nowhere abandons the gesture silently. That is deliberate: the user changed their
    // mind, and a message about an edit they did not make is noise.
    event->accept();
}

void NodeGraphView::frame_graph() {
    const QRectF bounds = scene_->itemsBoundingRect();
    if (bounds.isEmpty() || viewport()->width() <= 0 || viewport()->height() <= 0) return;

    // The scene rect is set from the **padded** bounds, not the raw ones, and the difference is what made a
    // node clip its port stub: `itemsBoundingRect` for a node starting at `kMargin` returns something whose
    // left edge is `kMargin - kPortRadius`, so the scene began just left of the stub. Deriving both the scene
    // rect and the framing from one padded rectangle is what keeps the two from disagreeing about where the
    // graph's edge is.
    const QRectF padded = bounds.adjusted(-kMargin, -kMargin, kMargin, kMargin);
    scene_->setSceneRect(padded);
    fitInView(padded, Qt::KeepAspectRatio);

    // Two clamps, and they pull in opposite directions on purpose.
    //
    // **Never magnify past 1:1.** `fitInView` scales up to fill, so without this a graph of two
    // nodes would be drawn larger than its authored size -- and a node editor whose node size
    // depends on how many nodes exist is one whose layout cannot be reasoned about.
    if (transform().m11() > 1.0) {
        resetTransform();
        centerOn(padded.center());
    }

    // **Never shrink below a legibility floor either.** This is the second half of the finding
    // the sizes above were changed for: framing a graph that does not fit used to scale it down
    // without limit, and the point of "large type" is lost the moment the view undoes it. So a
    // graph wider than the viewport is shown at `kMinimumScale` and **scrolled** instead.
    //
    // The trade is stated rather than hidden: the whole graph is no longer on screen at once in
    // that case, `all_nodes_are_visible()` reports it honestly, and a zoom-to-fit is the call a
    // user makes when seeing everything matters more than reading it. Choosing "smaller and
    // complete" as the *default* is what produced the original complaint.
    //
    // Centring is on the **padded** rect here too. `centerOn(bounds.center())` left the graph
    // slightly off-centre in the visible window, because the padded rect the frame was computed
    // from is what the viewport is actually showing.
    if (transform().m11() < kMinimumScale) {
        resetTransform();
        scale(kMinimumScale, kMinimumScale);
        centerOn(padded.center());
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

QSizeF NodeGraphView::authored_node_size() noexcept {
    const NodeMetrics& m = metrics();
    return QSizeF{m.width, m.height};
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

void NodeGraphView::select_node(qp::graph::NodeId node) {
    const auto it = node_items_.find(key_of(node));
    if (it == node_items_.end()) return;
    // Through the scene's own selection model, not a flag of our own: the highlight, `selectedItems()` and the
    // `selectionChanged` signal Qt emits all follow from it, so a test selecting a node exercises the same path a
    // click does. A second notion of "selected" is how a panel comes to disagree with a canvas.
    scene_->clearSelection();
    it->second->setSelected(true);
}

}  // namespace qp::views
