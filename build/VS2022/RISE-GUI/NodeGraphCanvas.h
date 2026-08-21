//////////////////////////////////////////////////////////////////////
//
//  NodeGraphCanvas.h - RISE UI redesign, left-panel "Graph" tab.
//
//  doc-88 Phase 3 S16 + S22 (docs/gui/NODE_GRAPH_CANVAS.md sect. 6):
//  the Windows/Qt mirror of the shipped macOS Painter/Material node
//  canvas (build/XCode/rise/RISE-GUI/App/NodeGraphCanvas.swift +
//  NodeGraphPalette.swift). CARRY-ONLY on this platform -- there is no
//  MSVC toolchain on the host that wrote this file; it is written to
//  match the macOS reference line-for-line and is owed a Windows build
//  + manual verification pass. See this file's own "PARITY TABLE"
//  block (bottom of NodeGraphCanvas.cpp) for the behavior-by-behavior
//  mapping and every place a Qt idiom forced a deviation from the Mac
//  shape.
//
//  SCOPE: both S16 (Phase A, read-only: node boxes with thumbnail/
//  fan-out badge/orphan badge/def-count pill, bezier wires, pan/zoom,
//  click-select, double-click def-focus) and S22 (Phase B, edit:
//  drag-to-reposition, drag-to-wire with live legality preview + a
//  refusal dialog carrying the Duplicate & Retry escape hatch, an
//  add-node search palette, keyboard delete/duplicate with the S20
//  verbs) are implemented together in this one slice, unlike Mac's
//  staged S15-then-S21 rollout -- there is no reason to carry two
//  separate Windows slices for a platform this whole file already
//  treats as "owed one MSVC pass" either way.
//
//  QGraphicsScene/QGraphicsView based (docs/gui/NODE_GRAPH_CANVAS.md
//  sect. 3's prescribed Windows widget technology). One GraphNodeItem
//  per node (QGraphicsItem, ItemIsMovable, paints its own thumbnail/
//  badges live from Theme:: tokens every paint -- see the LIVE
//  THEME-SWITCH CONTRACT in Theme.h; this class claims OutlinerWidget's
//  "rows are not widgets, drawn live" exemption for its item layer and
//  only needs restyleTheme() for the persistent header chrome), one
//  child GraphOutputHandleItem per node for the drag-to-wire hotspot
//  (a SIBLING gesture surface to the node's own move-drag, matching
//  the Mac file header's "not children of the node box... so a
//  wire-drag's own DragGesture never contends with the node body's
//  reposition-drag" rule -- here achieved by Qt's own child-item event
//  priority instead of a second top-level ZStack layer), and one
//  GraphWiresLayerItem drawing every wire + the live wire-drag draft in
//  a single low-z QPainterPath pass per repaint.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef NODEGRAPHCANVAS_H
#define NODEGRAPHCANVAS_H

#include <QWidget>
#include <QVector>
#include <QString>
#include <QPointF>
#include <QHash>
#include <QImage>
#include <QSet>

#include "ViewportBridge.h"   // for ViewportBridge::PainterGraph / RewireOutcome / etc.

class QLabel;
class QToolButton;
class QGraphicsScene;
class GraphNodeItem;
class GraphWiresLayerItem;
class NodeGraphView;

/// One reference-param slot, decoded from ViewportBridge::PainterGraphPort.
/// Plain value type -- the Qt mirror of the macOS file's private
/// `GraphCanvasPort` struct.
struct GraphPortData
{
    QString paramName;
    int     occurrence     = 0;
    int     otherNodeIndex = -1;   ///< -1 for a node-less (dangling) port
    QString otherName;
};

/// One node, decoded from ViewportBridge::PainterGraphNode + its laid-out
/// position. Mirrors the macOS file's private `GraphCanvasNode` struct,
/// MINUS the `position` field: on this platform a node's canonical live
/// position is `GraphNodeItem::pos()` (QGraphicsItem's own geometry), not
/// a value carried alongside the model data -- see NodeGraphCanvas.cpp's
/// parity table, "Live drag position source of truth."
struct GraphNodeData
{
    quint64 handle       = 0;
    int     index        = -1;     ///< this node's position in the fetched snapshot's node array -- what otherNodeIndex indexes into
    QString name;
    QString chunkKeyword;
    int     category     = -1;     ///< RISE::ChunkCategory ordinal: 0 Painter, 1 Function, 2 Material
    int     defCount     = 0;
    QPointF position;               ///< last-known-good position (seed for the item + revert-on-failure target)
    QVector<GraphPortData> outEdges;
    QVector<GraphPortData> inEdges;

    bool isRampPainter() const { return chunkKeyword == QLatin1String("ramp_painter"); }
    /// sect. 5 item: "a fan-out badge when a node has >1 in-edges."
    bool isShared() const { return inEdges.size() > 1; }
    /// Snapshot-derived orphan badge -- see NodeGraphCanvas.cpp's parity
    /// table entry for why this is recomputed every fetch rather than
    /// carried as a one-shot rewire-outcome event. Materials are excluded
    /// (a Material is this graph's natural root; zero in-edges there is
    /// normal, not an orphan).
    bool isOrphaned() const { return inEdges.isEmpty() && category != 2; }
};

/// User-requested slice: "show all nodes" vs "show only the selection's
/// subgraph". Ephemeral UI state ONLY -- never persisted (no QSettings
/// entry anywhere in this file); resets to All on every fresh launch.
/// Mirrors NodeGraphCanvas.swift's own GraphViewScope.
enum class GraphViewScope { All, Focused };

/// doc-88 Phase 3 S16/S21/S22: the Windows Painter/Material node canvas.
/// A persistent left-panel tab widget (mirrors OutlinerWidget/
/// EnvironmentPanel's lifetime discipline): built once, shows nothing
/// until setBridge() gives it a live scene, borrow cleared to nullptr on
/// scene teardown.
class NodeGraphCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit NodeGraphCanvas(QWidget* parent = nullptr);

    /// Borrows the bridge; safe to call again with a new pointer (or
    /// nullptr) on scene reload/teardown. Clears the canvas immediately
    /// on a null bridge (mirrors OutlinerWidget::setBridge).
    void setBridge(ViewportBridge* bridge);

    /// "Reveal in scene file" (item 3)'s SAME bridgeInteractingEnabled
    /// term -- gates the "+" add-node button, drag-to-reposition's write,
    /// drag-to-wire's commit, and the delete/duplicate verbs, all of
    /// which take the controller's commit mutex. Mirrors
    /// OutlinerWidget::setSceneEditable / Mac's isSceneEditableForAgents
    /// gate on every S21 mutation.
    void setSceneEditable(bool editable);

public slots:
    /// Re-pull the painter/material graph (epoch-gated -- a plain
    /// selection-only bump costs one integer compare, not a bridge call,
    /// the SAME idiom ViewportProperties::refresh / OutlinerWidget::
    /// refresh's epoch gate already uses -- see NodeGraphCanvas.cpp's
    /// parity table for why this file does NOT also carry Mac's 250ms
    /// GCD debounce). Called on scene load and on every preview frame
    /// (cheap when nothing changed), the SAME imageUpdated-driven cadence
    /// OutlinerWidget::refresh rides.
    void refresh();

    /// Unconditional re-pull, bypassing the epoch gate -- the "Refresh"
    /// button and every self-mutation call site (create/rewire/delete/
    /// duplicate/reposition) that already knows the epoch just moved and
    /// wants the fresh snapshot on this call, not the next frame.
    void refreshForce();

signals:
    /// Fired after this widget calls a bridge selection mutator --
    /// MainWindow forwards this to ViewportProperties::refresh() so the
    /// single-entity inspector follows a graph-canvas pick immediately,
    /// the SAME division of labor as OutlinerWidget::selectionActivated.
    void selectionActivated();

protected:
    // LIVE THEME-SWITCH CONTRACT (Theme.h): hook QEvent::PaletteChange
    // here and call restyleTheme() -- mirrors MainWindow::changeEvent.
    // Only the persistent header chrome needs it; the QGraphicsItem
    // layer reads Theme:: tokens live on every paint (OutlinerWidget's
    // exemption, see this file's header comment).
    void changeEvent(QEvent* e) override;

private:
    friend class GraphNodeItem;
    friend class GraphOutputHandleItem;
    friend class GraphWiresLayerItem;
    friend class NodeGraphView;

    // ---- reload -------------------------------------------------------
    void performReload(bool force);
    void applySnapshot(const ViewportBridge::PainterGraph& g);
    void prefetchThumbnails();
    void updateHeaderCounts();

    // ---- view-scope toggle (user-requested slice: "All" vs "Focused") -
    /// Re-derive the (category, name) the Focused view should root its
    /// subgraph at. Returns false when there is nothing to focus on (no
    /// selection). See NodeGraphCanvas.cpp's own comment for the
    /// deliberate TWO-SOURCE split (shared bridge selection for Object,
    /// THIS canvas's own m_selectedHandle for a Painter/Function/Material
    /// node) and why an outliner-driven Material/Painter pick does NOT
    /// count.
    bool currentFocusTarget(int& outCategory, QString& outName) const;
    /// The Focused-view fetch path, called from performReload() when
    /// m_viewScope == Focused and a target was resolved. `force` bypasses
    /// the cheap target-identity gate (mirrors performReload's own
    /// `force`). See NodeGraphCanvas.cpp's own comment for the degrade
    /// handling (leave the cheap gate uncommitted, no timer needed --
    /// this canvas re-derives every preview frame already).
    void performFocusedReload(int category, const QString& name, bool force);
    void onViewScopeToggled(bool checked);

    // ---- selection / def-focus (sect. 5 interaction minimums 1 & 2) ---
    void selectNode(const GraphNodeData& node);
    void setSelectedHandle(quint64 handle, bool valid);
    /// A double-click always selects -- see NodeGraphCanvas.cpp's parity
    /// table for why this platform's def-focus stops at "select the
    /// owning node" (no Qt equivalent of Mac's def-row scroll-anchor
    /// exists in ViewportProperties today).
    void openDefs(const GraphNodeData& node);

    // ---- object-pick spotlight (viewport/outliner -> canvas) -----------
    /// Re-derive m_spotlightHandles from the CURRENT shared (viewport/
    /// outliner) selection and re-apply it to every live GraphNodeItem.
    /// Called from THREE places: (a) performReload() on EVERY call (this
    /// widget's own per-frame refresh() cadence, see NodeGraphCanvas.h's
    /// class-level comment) -- independent of performReload's own epoch
    /// gate, since a plain Object pick does NOT bump
    /// ViewportBridge::sceneEpoch() (only a structural mutation does --
    /// SceneEditController::SceneEpoch's own comment), so this cannot
    /// piggyback on that early-return the way the structural graph refetch
    /// does; (b) selectNode(), so clicking a canvas node clears a live
    /// spotlight IMMEDIATELY rather than waiting for the next preview
    /// frame; (c) MainWindow's own explicit-follow wiring on a VIEWPORT/
    /// OUTLINER pick (selectionActivated's existing consumer,
    /// MainWindow.cpp), which is what makes a plain object pick with NO
    /// render in flight (so no imageUpdated to ride) actually light the
    /// canvas up at all.
    ///
    /// Internally cheap-exits BEFORE the expensive selectionRowName() walk
    /// using a CHEAP (selectionCategory(), selectionName()) pre-check --
    /// see the .cpp's own comment for why the two-tier check exists and why
    /// `forceReapply` (a just-rebuilt node set, whose items are all new and
    /// therefore all un-spotlit until this runs) bypasses only the cheap
    /// gate, never the auto-scroll-only-on-actual-change rule.
    ///
    /// CRITICAL (per NODE_GRAPH_CANVAS.md's own interaction contract):
    /// never calls ViewportBridge::setSelection. The shared selection
    /// stays on the object the properties panel is inspecting; this
    /// canvas only LOOKS at it, it never claims it.
    void refreshSpotlight(bool forceReapply);
    void applySpotlightToItems();

    // ---- drag-to-reposition (S21/S22) ----------------------------------
    // Takes the ITEM directly (not a handle/name pair): on a write
    // failure the item must snap back to its last-known-good position,
    // and on success it must adopt `pos` as the new known-good position
    // -- both are operations on the item itself, so the item is the
    // natural parameter (see GraphNodeItem::commitKnownGoodPosition /
    // revertToKnownGoodPosition).
    void commitNodeMove(GraphNodeItem* item, const QPointF& pos);
    /// Called from GraphNodeItem::itemChange on every incremental move
    /// (base-class-driven, since ItemIsMovable) -- repaints the wires
    /// layer so a wire tracks its node in real time during a drag. This
    /// is Qt's own item geometry firing the notification; there is no
    /// separate "live drag override" array (see NodeGraphCanvas.cpp's
    /// parity table entry on why Mac needs one and Qt does not).
    void notifyNodeGeometryChanged();

    // ---- drag-to-wire (S21/S22) -----------------------------------------
    struct WirePortTarget
    {
        quint64 nodeHandle   = 0;
        QString nodeName;
        int     nodeCategory = -1;
        QString paramName;
        int     occurrence   = 0;
        QPointF anchor;
    };
    struct WireDragState
    {
        quint64 sourceHandle   = 0;
        QString sourceName;
        int     sourceCategory = -1;
        QPointF currentPoint;
        bool    hasTarget      = false;
        quint64 targetHandle   = 0;
        QString targetName;
        int     targetCategory = -1;
        QString targetParam;
        int     targetOccurrence = 0;
        QPointF targetAnchor;
        int     verdictLegal   = -1;   ///< -1 unknown, 0 illegal, 1 legal
        QString verdictMessage;
    };
    void beginWireDrag(const GraphNodeData& source, const QPointF& scenePt);
    void updateWireDrag(const QPointF& scenePt);
    void endWireDrag(const QPointF& scenePt);
    bool findPortTarget(const QPointF& scenePt, WirePortTarget& outTarget) const;
    void commitRewire(quint64 sourceHandle, const QString& sourceName, int sourceCategory,
                       quint64 targetHandle, const QString& targetName, int targetCategory,
                       const QString& param, int occurrence);
    void showWireRefusal(const ViewportBridge::RewireOutcome& outcome,
                          quint64 sourceHandle, const QString& sourceName, int sourceCategory,
                          quint64 targetHandle, const QString& targetName, int targetCategory,
                          const QString& param, int occurrence);
    void updateStatusLabel();
    void hideStatusLabel();
    /// Keeps the floating wire-drag status label bottom-centered over the
    /// view's viewport -- called on every view resize (NodeGraphView is a
    /// friend so it can reach this directly) and whenever the label's
    /// text changes.
    void repositionStatusLabel();

    // ---- delete / duplicate (S20 verbs, wired S21/S22) ------------------
    GraphNodeData* selectedNodeMutable();
    void deleteSelected();
    void duplicateSelected();

    // ---- add-node palette (S18/S21/S22) ---------------------------------
    void openAddNodeDialog();

    // ---- thumbnails (S10 Qt carry: ViewportBridge::painterPreview /
    // rampStripPreview) ----------------------------------------------------
    QImage fetchThumbnail(const GraphNodeData& node) const;
    const QImage& thumbnailFor(const QString& name) const;

    // ---- geometry helpers -------------------------------------------------
    qreal currentScale() const;

    // LIVE THEME-SWITCH CONTRACT point 2/4: persistent-chrome restyle +
    // re-entrancy guard, uniform with every other panel in this app.
    void restyleTheme();
    bool m_themeReady = false;
    int  m_themeEpochSeen = -1;

    ViewportBridge* m_bridge = nullptr;
    bool m_sceneEditable = false;

    // ---- header chrome ------------------------------------------------
    QLabel*      m_titleLabel = nullptr;
    QLabel*      m_countLabel = nullptr;
    QToolButton* m_addBtn     = nullptr;
    QToolButton* m_refreshBtn = nullptr;
    /// User-requested slice: "All" vs "Focused" view-scope toggle --
    /// checkable QToolButton (this app's own idiomatic control for a
    /// two-state header toggle; a segmented Mac-style control has no
    /// direct single-widget Qt equivalent as cheap as a checkable button).
    QToolButton* m_viewScopeBtn = nullptr;
    QWidget*     m_header     = nullptr;   ///< bottom-bordered, mirrors MainWindow's left-panel tabStrip
    QLabel*      m_statusLabel = nullptr;   ///< wire-drag status line, floats over the view

    // ---- graphics scene -------------------------------------------------
    QGraphicsScene*       m_scene      = nullptr;
    NodeGraphView*        m_view       = nullptr;
    GraphWiresLayerItem*  m_wiresLayer = nullptr;

    QVector<GraphNodeData> m_nodes;       ///< canonical model, one entry per fetched node (index-addressed, mirrors otherNodeIndex)
    QVector<GraphNodeItem*> m_nodeItems;  ///< SAME order/index as m_nodes

    mutable QHash<QString, QImage> m_thumbnailCache;   ///< keyed by painter/function name, cleared+refilled per applySnapshot()

    unsigned int m_lastFetchedEpoch = 0;
    bool         m_hasFetchedOnce   = false;
    bool         m_hasCentered      = false;

    quint64 m_selectedHandle      = 0;
    bool    m_selectedHandleValid = false;

    // ---- view-scope toggle state (user-requested slice) -----------------
    GraphViewScope m_viewScope = GraphViewScope::All;
    /// Cheap target-identity gate for the Focused fetch -- mirrors
    /// m_lastSpotlightCategory/m_lastSpotlightSelectionName's own
    /// two-tier-gate lesson: this canvas re-derives on EVERY preview
    /// frame, so performFocusedReload must not pay a full applySnapshot
    /// rebuild when the resolved target hasn't changed. Committed ONLY on
    /// a non-degraded outcome -- see performFocusedReload's own comment.
    bool    m_lastFocusedHasTarget = false;
    int     m_lastFocusedCategory  = -1;
    QString m_lastFocusedName;

    // ---- object-pick spotlight state ------------------------------------
    QSet<quint64> m_spotlightHandles;
    /// Last selection CATEGORY `refreshSpotlight` actually acted on, plus
    /// the CHEAP `selectionName()` value paired with it -- the cheap
    /// early-exit key checked BEFORE the expensive `selectionRowName()`
    /// walk (see `refreshSpotlight`'s own comment). `Category::None` +
    /// empty name is the initial (never-run) state.
    ViewportBridge::Category m_lastSpotlightCategory = ViewportBridge::Category::None;
    QString                  m_lastSpotlightSelectionName;
    /// The RESOLVED row name (`selectionRowName()`) the spotlight was last
    /// computed for -- what actually gates the auto-scroll (a `forceReapply`
    /// pass with the SAME row name must re-apply spotlight state to the new
    /// items without re-scrolling; see `refreshSpotlight`'s own comment).
    QString                  m_lastSpotlightObjectName;

    bool          m_wireDragActive = false;
    WireDragState m_wireDrag;
};

#endif // NODEGRAPHCANVAS_H
