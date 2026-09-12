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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class QGraphicsLineItem;

namespace qp::views {

class NodeItem;
class EdgeItem;

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
     * @brief Scales and centres the view so the whole graph is visible, within the legibility floor.
     *
     * ## Why this exists, and why centring was not enough
     *
     * `centerOn` scrolls a viewport over the scene at the current scale, so a graph **wider than
     * the viewport is clipped wherever you centre it**. `default_position` lays nodes out in rows
     * of four, which is wider than this canvas on a 1280-wide window -- and the running shell
     * therefore showed one node of three while its own status line reported "nodes 3 | edges 2".
     * The scene held all three; the view could not show them.
     *
     * `fitInView` with `KeepAspectRatio` shows the whole graph. It is also what makes the view's
     * contents deterministic, which is what turned "the screenshot looks a bit off" into a
     * finding instead of a matter of opinion.
     *
     * ## Two clamps, and the second one is newer
     *
     * Never magnify past 1:1, so a node's size does not depend on how many exist. And never shrink
     * below `kMinimumScale`: framing a graph that does not fit used to scale it down without limit,
     * which undoes the point of choosing a legible type size. A graph wider than the viewport is
     * therefore shown at the floor and **scrolled**, and `all_nodes_are_visible()` reports that
     * honestly rather than the view pretending otherwise.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         the scene exists and the viewport has a size
     * @post        The whole scene rect is inside the viewport when it fits at or above `kMinimumScale`
     * @invariant   The scale is in `[kMinimumScale, 1.0]`
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.canvas.whole_graph_is_visible
     */
    void frame_graph();

    /// @brief The smallest scale `frame_graph` will choose. Public so a caller can state the same floor.
    ///
    /// `0.8` rather than `1.0`: a graph has to be substantially wider than the viewport before any
    /// shrinking happens at all, and 80% of a 10 pt title is still comfortably readable -- which is what the
    /// finding was about. A graph that needs less than this is scrolled instead.
    static constexpr qreal kMinimumScale = 0.85;

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

    /**
     * @brief Selects `node` as if the user had clicked it, and reports it.
     *
     * The selection a delete or a disconnect acts on. Exposed so a test can reach those two operations without
     * synthesising a click: what is worth asserting is the command and the resulting items, not Qt's event
     * delivery -- the same reasoning as `connect_ports`.
     *
     * @param node The node to select.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         none
     * @post        `selected_node()` returns `node` when it exists in the graph, and the item is highlighted
     * @invariant   `node_selected` is emitted, so a property panel follows exactly as it does for a click
     * @errors      Reports nothing: it either finds the item and selects it, or does nothing. Qt's
     *              `setSelected` is what may raise a signal, and a signal is not a failure
     * @complexity  O(nodes)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.nodegraph.delete_removes_the_node_and_its_edges
     */
    void select_node(qp::graph::NodeId node);

    /**
     * @brief Removes the node the user has selected, with every edge that touched it.
     *
     * The gesture-side counterpart of the command, exposed for the same reason `connect_ports` is: what is worth
     * asserting is the command and the resulting items, not Qt's event delivery.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         none
     * @post        With a node selected, the session holds one fewer node and its edges are gone; without a
     *              selection nothing happens and this returns false
     * @invariant   The edit goes through the session, so it is undoable and every panel is told
     * @errors      A refusal emits `mutation_failed` and returns false
     * @complexity  O(nodes + edges) through the session's notification
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.nodegraph.delete_removes_the_node_and_its_edges
     */
    [[nodiscard]] bool delete_selection();

    /**
     * @brief Removes the edge feeding the selected node's first fed input.
     *
     * The counterpart to drawing a connection, and it exists because an input holds at most one edge: without a
     * way to remove one, a mis-drawn wire can only be undone from the Edit menu -- which means the user has to
     * know that the connection they just drew is an undoable edit.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         none
     * @post        With a node whose input is fed, that edge is gone; otherwise nothing happens
     * @invariant   The edit goes through the session
     * @errors      A refusal emits `mutation_failed` and returns false
     * @complexity  O(edges)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.nodegraph.delete_removes_the_node_and_its_edges
     */
    [[nodiscard]] bool disconnect_selection();

    /**
     * @brief Scales the view by `factor`, keeping the viewport's centre fixed.
     *
     * Bounded by `kMinimumScale` and `kMaximumScale`. The lower bound is the **same** legibility floor
     * `frame_graph` respects, so no interactive gesture can undo the type size either -- which is the whole point
     * of having a floor rather than a preference.
     *
     * @param factor The multiplier. `> 1` zooms in.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         `factor > 0`
     * @post        The scale is in `[kMinimumScale, kMaximumScale]` and the same scene point is still centred
     * @invariant   Never magnifies past `kMaximumScale` nor shrinks below `kMinimumScale`
     * @errors      May allocate through Qt's scrollbar geometry in `centerOn`; the clamps mean a call at
     *              either limit is a no-op
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.nodegraph.the_canvas_can_be_panned_and_zoomed
     */
    void zoom_by(qreal factor);

    /// @brief The largest scale the interactive zoom reaches. Past this one node fills the window.
    static constexpr qreal kMaximumScale = 2.5;

    /// @brief The scene point at the centre of the viewport, and half of what `zoom_by` anchors on.
    [[nodiscard]] QPointF viewport_centre_in_scene() const;

    /**
     * @brief Brings `node` into view **without** changing the zoom.
     *
     * `ensureVisible` with a margin rather than `centerOn`: centring on a node the user just created moves the
     * whole picture, and a canvas that jumps whenever something is added is one where a user loses their place.
     *
     * @param node The node to reveal.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         none
     * @post        The node's box is inside the viewport, or the view is already at its scroll limit
     * @invariant   The scale is unchanged
     * @errors      May allocate through Qt's scrollbar geometry in `ensureVisible`
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.nodegraph.the_canvas_can_be_panned_and_zoomed
     */
    void reveal(qp::graph::NodeId node);

    /**
     * @brief The current zoom factor: 1.0 is a node drawn at its authored size.
     *
     * Exposed because "how large is the text" is otherwise only answerable by looking, and charter C5 asks for
     * large type as an acceptance criterion. A test can then hold the promise `frame_graph` makes -- that it
     * never drops below `kMinimumScale` -- instead of asserting the numbers inside the paint code.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        A positive scale, equal to the transform's horizontal scale
     * @invariant   `>= kMinimumScale` for any view `frame_graph` has framed
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.canvas.a_node_box_holds_its_type
     */
    [[nodiscard]] qreal scale_factor() const noexcept { return transform().m11(); }

    /**
     * @brief The size a node box is drawn at, in graph units, so a test can hold the type size to it.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        The box the paint code uses, from the fonts this platform resolved
     * @invariant   The same value for every node in a process
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      Depends on the resolved fonts
     * @frozen      no
     * @tests       qt.views.canvas.a_node_box_holds_its_type
     */
    [[nodiscard]] static QSizeF authored_node_size() noexcept;

    /// @brief The most recent mutation error, so a view can report it.
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    /**
     * @brief The endpoints of the nth drawn edge, in scene coordinates, for a test or a screenshot check.
     *
     * Exposed because "does the line touch the port it claims to join" is otherwise only answerable by looking at
     * pixels. The pair is `(from, to)`, and an out-of-range index yields two equal points rather than throwing.
     *
     * @param index Which edge, in the order `rebuild` drew them.
     *
     * @ownership   owns the returned pair
     * @thread      ui
     * @pre         none
     * @post        `first` is the output stub, `second` the input stub
     * @invariant   Both points lie on the bounding box edges of the two nodes it joins
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.nodegraph.an_edge_ends_on_its_ports
     */
    [[nodiscard]] std::pair<QPointF, QPointF> edge_endpoints(std::size_t index) const noexcept;

    /**
     * @brief Whether a connection is being drawn by hand right now.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        none
     * @invariant   True from a press on a port stub until the release that ends the gesture
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.nodegraph.a_dragged_connection_joins_two_ports
     */
    [[nodiscard]] bool is_drawing_connection() const noexcept { return drag_from_ != nullptr; }

    /**
     * @brief Draws a connection between two ports, as the mouse gesture does.
     *
     * The same code path the drag ends in, exposed so a test can exercise the connection **without** synthesising
     * mouse events: `QTest`'s mouse helpers need a mapped window and a platform plugin, and what is worth
     * asserting here is the command and the resulting line, not Qt's event delivery.
     *
     * @param from_node  The node whose output leaves.
     * @param from_port  The output port number.
     * @param to_node    The node whose input arrives.
     * @param to_port    The input port number.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         none
     * @post        On success the session holds one more edge and the canvas one more line
     * @invariant   Refused exactly when the session's command bus refuses it
     * @errors      Returns whether the connection was made; a refusal emits `mutation_failed`
     * @complexity  O(nodes + edges) through the session's notification
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.nodegraph.a_dragged_connection_joins_two_ports
     */
    bool connect_ports(qp::graph::NodeId from_node, qp::graph::PortIndex from_port,
                       qp::graph::NodeId to_node, qp::graph::PortIndex to_port);

protected:
    /**
     * @brief Starts a connection when the press lands on a port stub, and otherwise lets the node be dragged.
     *
     * ## Why the canvas draws connections at all
     *
     * Until this existed the only way to connect two nodes was through the session's command bus from outside --
     * a test helper, or a future script. A user looking at two boxes had no gesture that joined them, so the
     * demo graph arrived pre-wired and nothing could be rewired. A node editor where the wires can only be drawn
     * by a program is a diagram, not an editor.
     *
     * The press is checked against the **stubs** rather than the whole box, so dragging a node by its body still
     * moves it: a gesture that connected on any press would make every drag an accidental edit.
     *
     * @ownership   owns the gesture's preview line
     * @thread      ui
     * @pre         none
     * @post        Either a connection gesture is in flight, or the event reached the base class (a node drag)
     * @invariant   A gesture in flight has a non-null `drag_from_`
     * @errors      no-throw: Qt event handlers must not throw through the event loop
     * @complexity  O(nodes x ports) for the hit test
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.nodegraph.a_dragged_connection_joins_two_ports
     */
    void mousePressEvent(QMouseEvent* event) override;

    /// @brief Moves the preview line, and highlights the port it would land on.
    void mouseMoveEvent(QMouseEvent* event) override;

    /// @brief Finishes the gesture: connects, or abandons if the release is not on a port.
    void mouseReleaseEvent(QMouseEvent* event) override;

    /// @brief Zooms towards the viewport centre, bounded by the legibility floor.
    void wheelEvent(QWheelEvent* event) override;

    /// @brief Delete removes the selected node; F frames the graph.
    void keyPressEvent(QKeyEvent* event) override;

    /**
     * @brief How far from a stub a press still counts, in **device** pixels.
     *
     * Converted to scene units by the current zoom, so the grab radius is the same distance under the mouse at
     * every scale instead of growing when the view is zoomed out.
     */
    static constexpr qreal kPortGrabPixels = 12.0;

Q_SIGNALS:
    /// @brief Emitted after a selection change, so a property panel can follow.
    void node_selected(qp::graph::NodeId node);

    /// @brief Emitted when a mutation the canvas attempted was refused.
    void mutation_failed(const QString& reason);

private:
    /// @brief The session listener that keeps the canvas in step.
    class Bridge;

    /// @brief Recomputes the endpoints of every edge touching `moved`. Called on each drag step.
    void update_edges_for(const NodeItem& moved);

    qp::authoring::Session& session_;
    const qp::graph::NodeTypeRegistry& catalog_;
    qp::authoring::Document& document_;
    QGraphicsScene* scene_ = nullptr;
    std::unique_ptr<Bridge> bridge_;
    std::unordered_map<std::uint64_t, NodeItem*> node_items_;
    /// Every edge item, so a node drag can refresh the ones that touch it. Borrowed: the scene owns them.
    std::vector<EdgeItem*> edge_items_;
    /// The last cursor position of a middle-button pan, in device pixels.
    QPoint last_pan_pos_{};
    /// The connection being drawn by hand, if any. See `mousePressEvent`.
    NodeItem* drag_from_ = nullptr;
    qp::graph::PortIndex drag_from_port_ = qp::graph::kNoPort;
    QGraphicsLineItem* drag_line_ = nullptr;
    /// Whether the initial framing has happened. See rebuild().
    bool framed_ = false;
    /// Whether a deferred framing is in flight. See rebuild().
    bool pending_frame_ = false;
    std::string last_error_;
};

}  // namespace qp::views
