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

#include "ViewportBridge.h"

class QTimer;

class ViewportWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportWidget(ViewportBridge* bridge, QWidget* parent = nullptr);

public slots:
    void setImage(const QImage& image);

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

private slots:
    /// 500ms poll of ViewportBridge::getInteractiveRegion -- draws the
    /// persistent region box + "REGION" badge once a region is set
    /// (independent of who set it: this widget's own drag, or a future
    /// non-UI caller).  Mirrors TopBar's own 500ms refinement-state poll
    /// cadence.
    void pollRegionState();

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
    QPointF navBallCenter() const;
    void    navControlRects(QRectF& outHome, QRectF& outSet,
                            QRectF& outStamp, QRectF& outExit) const;
    void    paintNavOverlay(QPainter& p);
    bool    handleNavClick(const QPointF& widgetPos);   // true == consumed
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
};

#endif // VIEWPORTWIDGET_H
