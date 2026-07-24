//////////////////////////////////////////////////////////////////////
//
//  ViewportWidget.h - QWidget that displays the live-preview image
//    and forwards mouse events to a ViewportBridge.
//
//  Mirrors the macOS ViewportNSView.
//
//////////////////////////////////////////////////////////////////////

#ifndef VIEWPORTWIDGET_H
#define VIEWPORTWIDGET_H

#include <QWidget>
#include <QImage>
#include <QRect>
#include <QSize>

#include "ViewportBridge.h"

class QTimer;
class QComboBox;
class QToolButton;
class QMenu;
class QLabel;
class QAction;

class ViewportWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportWidget(ViewportBridge* bridge, QWidget* parent = nullptr);

public slots:
    void setImage(const QImage& image);

    /// N-up multi-viewport (docs/gui/RENDER_MODES.md §7): a completed frame
    /// for a SECONDARY pane (1..ViewportBridge::kViewportPaneCount-1) --
    /// wired to ViewportBridge::paneImageUpdated.  Pane 0's frames keep
    /// arriving via setImage() above (ViewportBridge::imageUpdated).
    void setPaneImage(unsigned int pane, const QImage& image);

    /// N-up multi-viewport: wired to ViewportToolbar::layoutChanged --
    /// resyncs the pane grid immediately after a layout-picker click
    /// instead of waiting for the next 500ms poll tick.
    void onViewportLayoutChanged();

    /// user-review P2#7: re-run the pane layout after a top-level screen /
    /// DPI change.  A FIXED-SIZE window dragged to a different-DPI display
    /// fires no resizeEvent (logical size unchanged), so the device-pixel
    /// pane dims would otherwise stay stale.  MainWindow forwards its
    /// QEvent::ScreenChangeInternal here.  No-op while Single is active
    /// (recomputePaneLayout early-outs there).
    void refreshForDpiChange();

    /// Update the cursor displayed over the viewport to match the
    /// active tool.  Wired to ViewportToolbar::toolChanged.  Also
    /// records the active tool's category so `paintEvent` knows
    /// whether to draw the gizmo overlay on top of the image.
    void setActiveTool(ViewportTool t);

    /// Hide / show the gizmo overlay during production renders.
    /// Mirrors macOS `isProductionRendering` — the cached gizmo
    /// handles can be stale relative to the production rasterizer's
    /// current camera state, so drawing them on the finished image
    /// would look like a misaligned scribble.  Driven by MainWindow
    /// from `RenderEngine::onStateChanged`.
    void setProductionRendering(bool inProgress);

    /// Chat-inclusive scene-editability gate (mirrors the props / outliner /
    /// environment panels' setSceneEditable).  A chat/agent-driven render holds
    /// the controller's commit mutex for its whole duration but does NOT change
    /// RenderEngine::state, so `m_productionRendering` (driven by onStateChanged)
    /// stays false during it.  MainWindow drives this from the chat-aware
    /// `updateMenuActionStates` fan-out so the nav overlay's isFreeFlyActive() /
    /// hasHomeView() reads and the pointer* calls (all of which take the
    /// controller mutex) can't wedge the UI during a chat-driven render.
    void setSceneEditable(bool editable);

    /// RISE UI redesign (A4 region refinement): wired to
    /// ViewportToolbar::regionArmedChanged.  While armed, the next
    /// press-drag-release draws the region box instead of routing
    /// pointer events to the bridge.  Turning armed off mid-drag
    /// (Escape, or the toolbar itself disarming) cancels the drag
    /// without committing a region -- and, matching the Mac's
    /// `suppressPointerUntilUp` guard (see cancelRegionDrag()'s doc),
    /// swallows the rest of that physical mouse gesture instead of
    /// leaking a pointerMove/pointerUp to the bridge with no matching
    /// pointerDown.
    void setRegionArmed(bool armed);

signals:
    /// Escape pressed while armed/dragging -- MainWindow forwards this
    /// to ViewportToolbar::cancelRegionArm() so the chip's state stays
    /// truthful.  Also emitted (harmlessly) if Escape lands with no
    /// arm/drag in progress; the toolbar's cancelRegionArm() is a
    /// no-op in that case.
    void regionArmCancelled();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    /// N-up multi-viewport: recomputes pane rects + repositions chrome +
    /// pushes fresh SetPaneSurfaceDims on every geometry change.  A no-op
    /// (early-out) while Single is the active layout -- see
    /// recomputePaneLayout()'s doc for why Single stays on the pre-N-up
    /// path untouched.
    void resizeEvent(QResizeEvent* event) override;

private slots:
    /// 500ms poll of ViewportBridge::getInteractiveRegion -- draws the
    /// persistent region box + "REGION" badge once a region is set
    /// (independent of who set it: this widget's own drag, or a future
    /// non-UI caller).  Mirrors TopBar's own 500ms refinement-state poll
    /// cadence.
    void pollRegionState();

    /// N-up multi-viewport (docs/gui/RENDER_MODES.md §7): same 500ms
    /// cadence as pollRegionState() above (connected to the SAME QTimer) --
    /// resyncs the current layout, primary-pane marker, and each visible
    /// pane's mode-combo / vantage-button chrome from the bridge.  The
    /// safety net behind onViewportLayoutChanged()'s immediate resync
    /// (mirrors TopBar::refreshRenderModeCombo's own poll-driven self-heal
    /// rationale -- see that method's doc).
    void pollPaneChrome();

    /// A per-pane mode combo box changed -- sender()'s "pane" property
    /// identifies which one.  Same click-then-reread discipline as
    /// TopBar::onRenderModeComboChanged (the set CAN fail).
    void onPaneModeComboChanged(int index);
    /// A per-pane vantage menu action was triggered -- sender() (the
    /// QMenu) carries the "pane" property; the action itself carries the
    /// vantage choice via its own data (see buildPaneChrome()'s doc).
    void onPaneVantageAction(QAction* action);

private:
    QPointF surfacePoint(const QPointF& pos) const;
    QRect   imageDrawRect() const;
    void    updateCursorForPosition(const QPointF& pos);
    void    paintGizmoOverlay(QPainter& p, const QRect& drawRect, const QSize& surface);
    bool    gizmoOverlayActive() const;

    // Navigation axis-ball (Tier 2 §4): painted in widget space in the
    // top-right corner; a nub click snaps the view (SnapViewToAxis), and the
    // three labeled controls Go-Home / Set-Home / Back-to-camera drive the
    // free-fly ViewportPose.  All layout/hit-test math is shared C++.
    QPointF navBallCenter(unsigned int pane) const;
    void    navControlRects(unsigned int pane, QRectF& outHome, QRectF& outSet,
                            QRectF& outStamp, QRectF& outExit) const;
    void    paintNavOverlay(QPainter& p, unsigned int pane);
    bool    handleNavClick(const QPointF& widgetPos, unsigned int pane);   // true == consumed
    void    paintRegionOverlay(QPainter& p, const QRect& drawRect, const QSize& surface);
    void    cancelRegionDrag();

    ViewportBridge*  m_bridge = nullptr;
    QImage           m_image;
    Qt::CursorShape  m_toolCursor = Qt::ArrowCursor;
    ViewportTool     m_activeTool = ViewportTool::Select;
    bool             m_productionRendering = false;
    bool             m_sceneEditable = true;   // chat-inclusive; see setSceneEditable

    // ---- RISE UI redesign: region refinement drag + persistent overlay --
    bool    m_regionArmed = false;
    bool    m_regionDragging = false;
    QPointF m_regionDragStart;      // widget-local coords
    QPointF m_regionDragCurrent;    // widget-local coords

    // Set by cancelRegionDrag() when it cancels a drag WHILE the mouse
    // button is still physically down (Escape, or setRegionArmed(false)
    // disarming mid-drag) -- see cancelRegionDrag()'s doc for why the
    // rest of that physical gesture must be swallowed rather than
    // forwarded.  Cleared in mouseReleaseEvent (the gesture's natural
    // end) and defensively in mousePressEvent (bounds the flag's
    // lifetime to at most one gesture even if a mouseReleaseEvent is
    // ever lost).  Mirrors ViewportView.swift's `suppressPointerUntilUp`.
    bool    m_suppressPointerUntilRelease = false;

    bool         m_hasRegion = false;   // last poll's getInteractiveRegion() result
    unsigned int m_regionLeft = 0, m_regionTop = 0, m_regionRight = 0, m_regionBottom = 0;
    QRect        m_regionBadgeRect;     // last-painted badge rect, for click-to-clear hit-testing
    QTimer*      m_regionPollTimer = nullptr;

    // ==================================================================
    // N-up multi-viewport (docs/gui/RENDER_MODES.md §7).  SINGLE layout
    // (the default, and the only layout before this feature) deliberately
    // keeps using m_image / imageDrawRect() / surfacePoint() / the region
    // and gizmo/nav overlays exactly as written above -- every method in
    // this block is a no-op (or takes an explicit `m_layout == Single`
    // early-out) while Single is active, so single-viewport behaviour is
    // byte-identical to before this section existed (§7.5's "Single
    // layout = today's behavior exactly" requirement).
    // ==================================================================

    static constexpr int kPaneHeaderHeight = 22;   // per-pane chrome strip, in widget points
    static constexpr int kPaneGap          = 2;    // hairline gap between pane cells

    /// One pane's chrome: a mode combo (populated once from the registry,
    /// like TopBar's m_renderModeCombo), a vantage menu button, and a
    /// read-only primary marker.  Built once at construction for all
    /// kViewportPaneCount slots (mirrors the core's "always-present pane
    /// slots" model) and simply hidden for panes the active layout
    /// doesn't show.
    struct PaneChrome {
        QComboBox*   modeCombo   = nullptr;
        QToolButton* vantageBtn  = nullptr;
        QMenu*       vantageMenu = nullptr;
        QLabel*      primaryDot  = nullptr;
    };

    void    buildPaneChrome();
    /// Recomputes m_paneRects/m_paneImageAreaRects for the CURRENT widget
    /// size + m_layout, repositions/shows/hides the chrome widgets, and
    /// pushes SetPaneSurfaceDims for every visible pane -- EXCEPT pane 0
    /// while Single is active, which deliberately keeps relying on
    /// ViewportBridge::scaleFilmToFit's screen-fit sizing instead (see the
    /// block doc above).  Called from resizeEvent, onViewportLayoutChanged,
    /// and pollPaneChrome (when it notices the layout changed).
    void    recomputePaneLayout();
    /// Retry only surface dimensions measured by recomputePaneLayout but not
    /// yet accepted by the controller. The periodic pane-chrome poll covers
    /// hosted/direct renders that own admission without toggling the shell's
    /// scene-editable or production-render state.
    void    retryPendingPaneSurfaceDims();
    /// Multi-view mode preset -- see the .cpp for the per-slot roles.
    /// Applied on every layout change to visible secondary panes still
    /// showing "preview" (user customization survives).
    void    applyMultiViewModePreset();
    /// Generalized imageDrawRect() -- letterbox `img` inside `area`.
    QRect   paneImageDrawRect(const QImage& img, const QRect& area) const;
    /// Generalized surfacePoint() for pane `pane`: maps a widget-local
    /// point to that pane's own image-pixel space, using the pane's own
    /// LAST-RECEIVED FRAME size as the reference (there is no per-pane
    /// twin of cameraSurfaceDimensions() in the C-ABI, so -- like
    /// surfacePoint()'s own no-bridge fallback -- the most recent frame's
    /// pixel size is the best available stand-in; a fast drag can shrink
    /// it via preview-scale subsampling exactly like the single-viewport
    /// path already tolerates).
    QPointF paneSurfacePoint(unsigned int pane, const QPointF& widgetPos) const;
    /// Hit-test the visible panes' cell rects (N-up layouts only).
    /// Returns -1 when `widgetPos` falls in no pane (the inter-pane gap).
    int     paneAt(const QPointF& widgetPos) const;
    void    paintPaneGrid(QPainter& p);
    void    refreshPaneChromeState();

    ViewportBridge::ViewportLayout m_layout = ViewportBridge::ViewportLayout::Single;
    unsigned int m_visiblePaneCount = 1;
    QImage  m_paneImages[ViewportBridge::kViewportPaneCount];
    QRect   m_paneRects[ViewportBridge::kViewportPaneCount];           // whole cell, incl. chrome header
    QRect   m_paneImageAreaRects[ViewportBridge::kViewportPaneCount];  // cell minus header -- where the image draws
    PaneChrome m_paneChrome[ViewportBridge::kViewportPaneCount];
    /// The pane a pointer gesture is currently pinned to (gesture
    /// exclusivity, §7.3), or -1 between gestures.  Set on a successful
    /// OnPanePointerDown, cleared on the matching Up (mirrors the
    /// region-drag flag pattern above).
    int     m_activeGesturePane = -1;
    /// Cached per-pane surface dims last ACCEPTED by the controller, so
    /// resizeEvent/recomputePaneLayout only calls SetPaneSurfaceDims when
    /// the desired size differs. A refusal leaves the accepted cache
    /// unchanged so setSceneEditable(true) can retry after a render releases
    /// admission (mirrors the core's same-dim short-circuit convention
    /// referenced in RENDER_MODES.md §7.3's invalidation matrix).
    QSize   m_paneLastPushedDims[ViewportBridge::kViewportPaneCount];
    QSize   m_paneDesiredDims[ViewportBridge::kViewportPaneCount];
    /// user-review P2#2: panes the multi-view preset has already been
    /// applied to.  Applying it exactly once per pane (not "whenever the
    /// pane still reads preview") keeps an EXPLICIT Preview choice from
    /// being re-clobbered on every layout change.  Reset with the widget
    /// (recreated per scene load), mirroring the Mac @State twin.
    bool    m_panePresetApplied[ViewportBridge::kViewportPaneCount] = { false };
};

#endif // VIEWPORTWIDGET_H
