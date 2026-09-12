/**
 * @file node_graph_view.hpp
 * @brief The node editor: a canvas that renders the session's one graph.
 *
 * ## The rule this widget is written to respect
 *
 * `authoring::Session` owns the graph, the command bus and the undo stack, and
 * says a view holds the session and never a graph. So this widget owns **no graph
 * data at all**: the items it draws are rebuilt from `session.graph()` whenever
 * the session reports a change, and every edit goes back through
 * `session.apply(...)`.
 *
 * The reason is not tidiness. Two copies of a graph diverge without crashing: the
 * canvas would keep drawing the node the property panel already deleted, the user
 * would believe the canvas, and the experiment recorded would not be the one they
 * configured. A canvas that caches "its" node list is exactly that second copy.
 *
 * ## What lives on this side of the line
 *
 * Node positions do. They are not a property of the graph -- the graph says "a
 * spring is connected to a mass", not "and the spring is drawn at (120, 80)" --
 * so they belong to `authoring::document`'s per-view layout slots, keyed by view
 * id. This widget reads and writes the slot named `graph` and nothing else.
 *
 * ## Why QGraphicsView and not a custom QWidget with paintEvent
 *
 * Hit testing, item selection, coordinate transforms and scrolling are all
 * already solved by the graphics view framework, and writing them by hand is how
 * a canvas acquires the subtle bugs (a stale bounding rect, a transform applied
 * twice) that look like physics problems. The framework also lets an edge be a
 * real item with a real shape, which is what makes edges clickable.
 *
 * @ownership   owns (scene items; never graph data)
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   The item set always matches session.graph() after a change notification
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.nodegraph.items_match_graph,
 *              qt.views.nodegraph.positions_come_from_layout,
 *              qt.views.nodegraph.drag_moves_the_node_once
 */
#pragma once

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QPointF>

#include <qp/authoring/commands/session.hpp>
#include <qp/authoring/document/document.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace qp::views {

class NodeItem;

/// @brief The view id whose layout slot this canvas owns.
///
/// A named constant rather than a literal scattered through the file: the slot id
/// is the contract between this canvas and the document, and a typo in one place
/// would silently produce a canvas that never remembers where anything was.
inline constexpr const char* kGraphViewId = "graph";

/**
 * @brief A canvas that renders one session's graph and edits it through the bus.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         the session outlives the widget
 * @post        none
 * @invariant   Every mutation goes through authoring::Session::apply
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.nodegraph.items_match_graph
 */
class NodeGraphView final : public QGraphicsView {
    Q_OBJECT

public:
    /**
     * @brief Binds the canvas to a session, a catalog and a document.
     *
     * @ownership   observes all three; none is owned
     * @thread      ui
     * @pre         all three outlive this widget
     * @post        The canvas shows the session's current graph
     * @invariant   The widget never stores a graph of its own
     * @errors      May allocate while building scene items; allocation failure
     *              terminates rather than being reported, because a canvas that
     *              cannot be built has nowhere to report to
     * @complexity  O(nodes)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.nodegraph.items_match_graph
     */
    NodeGraphView(qp::authoring::Session& session, const qp::graph::NodeTypeRegistry& catalog,
                  qp::authoring::Document& document, QWidget* parent = nullptr);

    /// @brief Declared out of line because `Bridge` is incomplete here.
    ///
    /// A `std::unique_ptr` member whose type is incomplete at the point of
    /// instantiation makes the implicit destructor ill-formed: the compiler cannot
    /// generate `delete` for a type it cannot see. Declaring the destructor and
    /// defining it in the .cpp -- where `Bridge` is complete -- is the fix, and it
    /// is the standard idiom rather than a workaround.
    ~NodeGraphView() override;

    NodeGraphView(const NodeGraphView&) = delete;
    NodeGraphView& operator=(const NodeGraphView&) = delete;

    /// @brief The layout slot id this canvas reads and writes.
    [[nodiscard]] static const char* view_id() noexcept { return kGraphViewId; }

    /**
    /**
     * @brief Scales and centres the view so the whole graph is visible.
     *
     * ## Why this exists, and why centring was not enough
     *
     * `centerOn` scrolls a viewport over the scene at the current scale, so a graph **wider than
     * the viewport is clipped wherever you centre it**. `default_position` lays nodes out in rows
     * of four, 168 units apart, which is wider than this canvas on a 1280-wide window -- and the
     * running shell therefore showed one node of three while its own status line reported
     * "nodes 3 | edges 2". The scene held all three; the view could not show them.
     *
     * `fitInView` with `KeepAspectRatio` shows the whole graph. It is also what makes the view's
     * contents deterministic, which is what turned "the screenshot looks a bit off" into a
     * finding instead of a matter of opinion.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         the scene exists and the viewport has a size
     * @post        The whole scene rect is inside the viewport, at no more than 1:1
     * @invariant   Never magnifies past 1:1, so a node's size does not depend on how many exist
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.canvas.whole_graph_is_visible
     */
    void frame_graph();

    /**
     * @brief Whether every node item lies inside the viewport.
     *
     * A genuine query rather than a test hook: "can the user see the graph" is a question a
     * window manager, a screenshot check, or a future zoom-to-fit button all need to ask, and
     * deriving it from pixel inspection is how a viewport bug survives a green suite.
     *
     * The comparison is against `viewport()->rect()` mapped into scene coordinates, so a node
     * that is scrolled out of view is reported as not visible even though it exists.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         the scene and viewport exist
     * @post        True exactly when every NodeItem's bounding rect intersects the visible area
     * @invariant   An empty graph reports true, vacuously
     * @errors      noexcept
     * @complexity  O(nodes)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.canvas.whole_graph_is_visible,
     *              qt.views.canvas.unframed_view_reports_clipped
     */
    [[nodiscard]] bool all_nodes_are_visible() const noexcept;

    /**
     * @brief Rebuilds every item from the session's graph.
     *
     * Called on construction and after every change notification. It is a full
     * rebuild rather than a patch: a partial update has to know which commands
     * affect which items, and getting that mapping subtly wrong is how a canvas
     * ends up showing a node that no longer exists.
     *
     * @ownership   owns the items it creates
     * @thread      ui
     * @pre         the scene exists
     * @post        One NodeItem per graph node, one edge per graph edge
     * @invariant   items_match_graph() holds afterwards
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(nodes + edges)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.nodegraph.items_match_graph
     */
    void rebuild();

    /// @brief Number of node items currently drawn. Used by the tests.
    [[nodiscard]] int node_item_count() const noexcept;

    /// @brief Number of edge items currently drawn. Used by the tests.
    [[nodiscard]] int edge_item_count() const noexcept;

    /// @brief Whether the drawn items match the session's graph exactly.
    [[nodiscard]] bool items_match_graph() const noexcept;

    /// @brief Records `node`'s position into the document's layout slot.
    void remember_position(qp::graph::NodeId node, QPointF position);

    /// @brief The remembered position of `node`, or a default when it has none.
    [[nodiscard]] QPointF position_of(qp::graph::NodeId node) const;

    /// @brief The node the user currently has selected, if any.
    [[nodiscard]] std::optional<qp::graph::NodeId> selected_node() const;

    /// @brief The most recent mutation error, so a view can report it.
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

Q_SIGNALS:
    /// @brief Emitted after a selection change, so a property panel can follow.
    void node_selected(qp::graph::NodeId node);

    /// @brief Emitted when a mutation the canvas attempted was refused.
    void mutation_failed(const QString& reason);

private:
    /// @brief The session listener that keeps the canvas in step.
    class Bridge;

    qp::authoring::Session& session_;
    const qp::graph::NodeTypeRegistry& catalog_;
    qp::authoring::Document& document_;
    QGraphicsScene* scene_ = nullptr;
    std::unique_ptr<Bridge> bridge_;
    std::unordered_map<std::uint64_t, NodeItem*> node_items_;
    /// Whether the initial framing has happened. See rebuild().
    bool framed_ = false;
    /// Whether a deferred framing is in flight. See rebuild().
    bool pending_frame_ = false;
    std::string last_error_;
};

}  // namespace qp::views
