//////////////////////////////////////////////////////////////////////
//
//  ObjectGraphCanvas.h - RISE UI redesign, left-panel "Object Graph" tab.
//
//  S3 (Qt carry, MSVC-owed as usual): the Windows/Qt mirror of the
//  shipped macOS Object Graph canvas
//  (build/XCode/rise/RISE-GUI/App/ObjectGraphCanvas.swift). CARRY-ONLY
//  on this platform -- there is no MSVC toolchain on the host that wrote
//  this file; it is written to match the macOS reference line-for-line
//  and is owed a Windows build + manual verification pass. See
//  NodeGraphCanvas.cpp's own "PARITY TABLE" block (this file adds ONE
//  row there, row 30 -- kept as the single shared table rather than a
//  second one in this file) for the behavior-by-behavior mapping and
//  every place a Qt idiom forced a deviation from the Mac shape.
//
//  SCOPE: READ-ONLY phase 1, mirroring ObjectGraphCanvas.swift's own
//  scoping exactly -- pan/zoom, node cards (name, chunk keyword, a "xN"
//  repeat badge for repeatCount >= 1), bezier wires, an All/Focused
//  view-scope toggle with the sticky-object policy NodeGraphCanvas
//  already carries on this platform, a single-node spotlight for the
//  externally-picked object, click-to-select routing to the shared
//  selection. Explicitly NOT this phase: double-click (does nothing),
//  node dragging (this graph's layout is ALWAYS transient -- see
//  SceneEditController::ReadObjectGraphLaidOut's own header comment;
//  there is no sidecar partition to persist a drag into), add/rewire/
//  delete/duplicate.
//
//  QGraphicsScene/QGraphicsView based, same technology NodeGraphCanvas
//  uses for the SAME sect. 3 prescription. One ObjectGraphNodeItem per
//  node (QGraphicsItem, paints its own card live from Theme:: tokens
//  every paint -- OutlinerWidget's "rows are not widgets, drawn live"
//  exemption, same claim NodeGraphCanvas.h's own header makes), NO
//  per-node output-handle child item (no drag-to-wire exists here to
//  need one), one ObjectGraphWiresLayerItem drawing every wire in a
//  single low-z QPainterPath pass per repaint.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef OBJECTGRAPHCANVAS_H
#define OBJECTGRAPHCANVAS_H

#include <QWidget>
#include <QVector>
#include <QString>
#include <QPointF>

#include "ViewportBridge.h"   // for ViewportBridge::PainterGraph (reused verbatim -- see .cpp's own comment)

class QLabel;
class QToolButton;
class QGraphicsScene;
class ObjectGraphNodeItem;
class ObjectGraphWiresLayerItem;
class ObjectGraphView;

/// One reference-param slot, decoded from ViewportBridge::PainterGraphPort.
/// Byte-identical shape to NodeGraphCanvas.h's own GraphPortData -- kept
/// as a SEPARATE type (not a reuse of that one) because the two files are
/// independent Qt carry slices with no shared header of their own for
/// plain value types; duplicating this ~4-field struct is cheaper than
/// inventing a shared header for it.
struct ObjectGraphPortData
{
    QString paramName;
    int     occurrence     = 0;
    int     otherNodeIndex = -1;   ///< -1 for a node-less (dangling) port
    QString otherName;
};

/// One node, decoded from ViewportBridge::PainterGraphNode + its laid-out
/// position. Mirrors ObjectGraphCanvas.swift's own private
/// ObjGraphCanvasNode struct.
struct ObjectGraphNodeData
{
    quint64 handle       = 0;
    int     index        = -1;     ///< this node's position in the fetched snapshot's node array -- what otherNodeIndex indexes into
    QString name;
    QString chunkKeyword;
    /// RISE::ChunkCategory ordinal -- Geometry=5, Object=8
    /// (ChunkDescriptor.h; a rect_light/shape_light node already arrives
    /// re-categorized as Object=8 by BuildObjectGraphSeedsLocked_, so this
    /// file never needs to special-case those keywords itself).
    int     category     = -1;
    /// A standard_object's count_u * count_v repeat sugar; 0 when this
    /// chunk carries no counts at all. See SceneEditController::
    /// GraphNode::repeatCount's own header comment for the accepted
    /// count_u 0 ambiguity (both publish 0 here).
    int     repeatCount  = 0;
    QPointF position;               ///< last-known-good position (seed for the item -- no drag exists to revert FROM, kept only for symmetry with the layout fetch)
    QVector<ObjectGraphPortData> outEdges;
    QVector<ObjectGraphPortData> inEdges;
};

/// "All" vs "Focused" -- same semantics as NodeGraphCanvas.h's own
/// GraphViewScope, a SEPARATE enum for the same "no shared header for a
/// two-value type" reason ObjectGraphPortData's comment gives.
enum class ObjectGraphViewScope { All, Focused };

/// S3: the Windows Object Graph canvas -- sibling of NodeGraphCanvas,
/// same persistent-widget discipline (built once by MainWindow, shows
/// nothing until setBridge() gives it a live scene, borrow cleared to
/// nullptr on scene teardown).
class ObjectGraphCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit ObjectGraphCanvas(QWidget* parent = nullptr);

    /// Borrows the bridge; safe to call again with a new pointer (or
    /// nullptr) on scene reload/teardown. Clears the canvas immediately
    /// on a null bridge, and clears the sticky-focus memo (this canvas is
    /// PERSISTENT, so nothing else would ever reset a stale name from a
    /// previous scene -- mirrors NodeGraphCanvas::setBridge's identical
    /// reasoning).
    void setBridge(ViewportBridge* bridge);

    /// Reserved for a future mutation affordance -- nothing in THIS
    /// phase reads `m_sceneEditable` at all (there is no "+", no drag-
    /// to-wire commit, no delete/duplicate here to gate). Wired at the
    /// SAME MainWindow call site NodeGraphCanvas::setSceneEditable is,
    /// for lifecycle-wiring consistency with that sibling canvas, so a
    /// future edit affordance has nothing new to plumb.
    void setSceneEditable(bool editable);

public slots:
    /// Re-pull the object graph (epoch-gated in the All-view; Focused
    /// mode never epoch-gates -- see performReload's own comment). Called
    /// on scene load and on every preview frame (cheap when nothing
    /// changed), the SAME imageUpdated-driven cadence NodeGraphCanvas::
    /// refresh rides.
    void refresh();

    /// Unconditional re-pull, bypassing the epoch gate -- the "Refresh"
    /// button and the tab-switch-in call site.
    void refreshForce();

signals:
    /// Fired after this widget calls a bridge selection mutator --
    /// MainWindow forwards this to ViewportProperties::refresh() /
    /// OutlinerWidget::refresh() so the single-entity inspector and the
    /// outliner follow a graph-canvas pick immediately, the SAME division
    /// of labor NodeGraphCanvas::selectionActivated already establishes.
    void selectionActivated();

protected:
    // LIVE THEME-SWITCH CONTRACT (Theme.h): hook QEvent::PaletteChange
    // here and call restyleTheme() -- mirrors NodeGraphCanvas::changeEvent.
    void changeEvent(QEvent* e) override;

private:
    friend class ObjectGraphNodeItem;
    friend class ObjectGraphWiresLayerItem;
    friend class ObjectGraphView;

    // ---- reload -------------------------------------------------------
    void performReload(bool force);
    void applySnapshot(const ViewportBridge::PainterGraph& g);
    void updateHeaderCounts();

    // ---- view-scope toggle (FINAL sticky policy, ported from
    // NodeGraphCanvas's own review-round-settled shape -- see that
    // class's identically-named methods for the full rationale each of
    // these mirrors) -----------------------------------------------------
    void updateStickyFocusObject();
    bool currentFocusTarget(QString& outName) const;
    void performFocusedReload(const QString& name, bool force);
    void onViewScopeToggled(bool checked);

    // ---- selection (click-to-select only -- no double-click affordance,
    // no drag; see this file's own header "SCOPE" note) -------------------
    void selectNode(const ObjectGraphNodeData& node);
    void setSelectedHandle(quint64 handle, bool valid);

    // ---- object-pick spotlight (viewport/outliner -> canvas) -----------
    /// Re-derive m_spotlightHandle from the CURRENT shared selection and
    /// re-apply it to every live ObjectGraphNodeItem. A SINGLE node, not a
    /// closure (unlike NodeGraphCanvas's appearance-closure spotlight) --
    /// see ObjectGraphCanvas.swift's own header comment for why: this
    /// canvas's "All" view already shows every object, so the spotlight
    /// only needs to say "you are looking at THIS one." Gated behind its
    /// OWN cheap (selectionCategory(), selectionName()) pre-check before
    /// selectionRowName()'s O(rows) walk -- the SAME per-frame-poll
    /// invariant NodeGraphCanvas::updateStickyFocusObject's own gate
    /// exists for (this canvas's refresh() rides ViewportBridge::
    /// imageUpdated too), kept as a SEPARATE gate from the sticky-focus
    /// one because the two track different things (current selection vs.
    /// sticky memo) with different persistence.
    void refreshSpotlight(bool forceReapply);
    void applySpotlightToItems();

    // ---- geometry helpers -------------------------------------------------
    qreal currentScale() const;

    // LIVE THEME-SWITCH CONTRACT point 2/4: persistent-chrome restyle +
    // re-entrancy guard, uniform with NodeGraphCanvas.
    void restyleTheme();
    bool m_themeReady = false;
    int  m_themeEpochSeen = -1;

    ViewportBridge* m_bridge = nullptr;
    bool m_sceneEditable = false;   ///< reserved -- see setSceneEditable's own comment

    // ---- header chrome ------------------------------------------------
    QLabel*      m_titleLabel = nullptr;
    QLabel*      m_countLabel = nullptr;
    QToolButton* m_refreshBtn = nullptr;
    QToolButton* m_viewScopeBtn = nullptr;
    QWidget*     m_header     = nullptr;

    // ---- graphics scene -------------------------------------------------
    QGraphicsScene*             m_scene      = nullptr;
    ObjectGraphView*            m_view       = nullptr;
    ObjectGraphWiresLayerItem*  m_wiresLayer = nullptr;

    QVector<ObjectGraphNodeData>  m_nodes;       ///< canonical model, one entry per fetched node (index-addressed, mirrors otherNodeIndex)
    QVector<ObjectGraphNodeItem*> m_nodeItems;   ///< SAME order/index as m_nodes

    unsigned int m_lastFetchedEpoch = 0;
    bool         m_hasFetchedOnce   = false;
    bool         m_hasCentered      = false;

    quint64 m_selectedHandle      = 0;
    bool    m_selectedHandleValid = false;

    // ---- view-scope toggle state ----------------------------------------
    ObjectGraphViewScope m_viewScope = ObjectGraphViewScope::All;
    /// Cheap target-identity gate for the Focused fetch -- mirrors
    /// NodeGraphCanvas::m_lastFocusedHasTarget/Category/Name's own
    /// lockstep-commit lesson exactly (review-round P1 fix on that
    /// class, ported here rather than re-derived): committed ONLY on a
    /// non-degraded outcome, cleared TOGETHER with the sticky memo. This
    /// graph has no `category` axis (always Object), so there is no
    /// `m_lastFocusedCategory` twin to carry.
    bool    m_lastFocusedHasTarget = false;
    QString m_lastFocusedName;
    /// The STICKY focus memo -- Focused mode's subgraph target is ONLY
    /// EVER the last externally-selected Object (viewport/outliner),
    /// never a Geometry-node canvas click (a canvas click on an OBJECT
    /// node DOES update this -- see updateStickyFocusObject's own
    /// comment for why that asymmetry is correct and needs no special-
    /// casing). Written ONLY by updateStickyFocusObject();
    /// currentFocusTarget() reads it and nothing else. Explicitly cleared
    /// in setBridge() on every scene (re)load.
    bool    m_stickyFocusHasObject   = false;
    QString m_stickyFocusObjectName;
    /// Cheap `(selectionCategory(), selectionName())` pre-check gate for
    /// updateStickyFocusObject() -- ported from NodeGraphCanvas's own
    /// m_lastStickyFocusCategory/Name (review-round P2 fix on that
    /// class): this canvas re-derives on EVERY preview frame, so it must
    /// not re-pay the O(rows) selectionRowName() walk on every frame
    /// while an object merely sits selected through a long render.
    /// Cleared alongside the sticky memo in setBridge().
    ViewportBridge::Category m_lastStickyFocusCategory = ViewportBridge::Category::None;
    QString                  m_lastStickyFocusSelectionName;

    // ---- object-pick spotlight state ------------------------------------
    /// -1 (via m_spotlightHandleValid==false) when nothing is spotlit.
    /// SINGLE handle, not a set -- see refreshSpotlight's own comment.
    quint64 m_spotlightHandle      = 0;
    bool    m_spotlightHandleValid = false;
    /// Cheap gate for refreshSpotlight's own selectionRowName() walk --
    /// SEPARATE from m_lastStickyFocus* above (different purpose,
    /// different persistence -- see refreshSpotlight's own header
    /// comment). `Category::None` + empty name is the initial
    /// (never-run) state.
    ViewportBridge::Category m_lastSpotlightCategory = ViewportBridge::Category::None;
    QString                  m_lastSpotlightSelectionName;
};

#endif // OBJECTGRAPHCANVAS_H
