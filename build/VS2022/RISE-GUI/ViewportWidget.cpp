//////////////////////////////////////////////////////////////////////
//
//  ViewportWidget.cpp
//
//////////////////////////////////////////////////////////////////////

#include "ViewportWidget.h"
#include "ViewportBridge.h"
#include "Theme.h"

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWindow>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QTimer>

#include <algorithm>
#include <cmath>

ViewportWidget::ViewportWidget(ViewportBridge* bridge, QWidget* parent)
    : QWidget(parent)
    , m_bridge(bridge)
{
    setAutoFillBackground(true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);

    // RISE UI redesign (A4 region refinement): 500ms poll of the
    // bridge's interactive-region state, independent of who set it.
    m_regionPollTimer = new QTimer(this);
    m_regionPollTimer->setInterval(500);
    connect(m_regionPollTimer, &QTimer::timeout, this, &ViewportWidget::pollRegionState);
    m_regionPollTimer->start();
    pollRegionState();
}

void ViewportWidget::setImage(const QImage& image)
{
    m_image = image;
    update();
}

void ViewportWidget::setActiveTool(ViewportTool t)
{
    m_activeTool = t;
    // Map each tool to a Qt cursor.  Object Translate / Rotate / Scale
    // and standalone Scrub aren't surfaced in the toolbar but still
    // appear here in case the controller is driven from elsewhere
    // (tests, future tooling).
    switch (t) {
    case ViewportTool::Select:          m_toolCursor = Qt::ArrowCursor;     break;
    case ViewportTool::TranslateObject: m_toolCursor = Qt::SizeAllCursor;   break;
    case ViewportTool::RotateObject:    m_toolCursor = Qt::OpenHandCursor;  break;
    case ViewportTool::ScaleObject:     m_toolCursor = Qt::SizeFDiagCursor; break;
    case ViewportTool::OrbitCamera:     m_toolCursor = Qt::OpenHandCursor;  break;
    case ViewportTool::PanCamera:       m_toolCursor = Qt::SizeAllCursor;   break;
    case ViewportTool::ZoomCamera:      m_toolCursor = Qt::SizeVerCursor;   break;
    case ViewportTool::ScrubTimeline:   m_toolCursor = Qt::SizeHorCursor;   break;
    case ViewportTool::RollCamera:      m_toolCursor = Qt::SizeHorCursor;   break;
    }
    // Don't unconditionally setCursor on the whole widget — the
    // cursor should only be the tool cursor over the rendered image
    // area (the dark surround uses the parent's cursor / system
    // arrow).  The actual cursor swap happens in mouseMoveEvent /
    // leaveEvent, driven by the pointer's current position.
    unsetCursor();
    // Tool change can flip the gizmo shape (Translate → Rotate
    // switches arrows for rings); repaint so the overlay updates.
    update();
}

void ViewportWidget::setProductionRendering(bool inProgress)
{
    if (m_productionRendering == inProgress) return;
    m_productionRendering = inProgress;
    update();
}

void ViewportWidget::setSceneEditable(bool editable)
{
    if (m_sceneEditable == editable) return;
    m_sceneEditable = editable;
    if (!editable) {
        // A render now owns the scene: cancel any armed / in-progress region
        // interaction so a mid-render release can't commit it
        // (setInteractiveRegion -> KickRender -> mMutex) and it doesn't linger
        // armed across the render.
        m_regionArmed = false;
        m_regionDragging = false;
    }
    update();   // re-evaluate the nav-overlay draw gate at the new state
}

void ViewportWidget::setRegionArmed(bool armed)
{
    if (m_regionArmed == armed) return;
    m_regionArmed = armed;
    if (!armed) {
        cancelRegionDrag();
    }
    update();
}

void ViewportWidget::cancelRegionDrag()
{
    if (!m_regionDragging) return;
    m_regionDragging = false;
    // A drag was cancelled while the mouse button is still physically
    // down (m_regionDragging is only ever true between mousePressEvent's
    // region-armed branch -- which never forwards a pointerDown to the
    // bridge for this gesture -- and the matching mouseReleaseEvent, so
    // reaching this line WITH m_regionDragging true means that release
    // hasn't happened yet).  Qt keeps delivering mouseMoveEvent /
    // mouseReleaseEvent for the rest of this physical gesture regardless;
    // without this flag those trailing events would fall through to the
    // normal-tool branch and forward a pointerMove/pointerUp to the
    // bridge with NO matching pointerDown, corrupting the controller's
    // drag-anchor state.  Cleared in mouseReleaseEvent once the gesture
    // ends, and defensively in mousePressEvent so a lost
    // mouseReleaseEvent can't wedge future gestures shut.  Called from
    // both keyPressEvent's Escape handling and setRegionArmed(false) --
    // mirrors ViewportView.swift's `suppressPointerUntilUp`.
    m_suppressPointerUntilRelease = true;
    update();
}

void ViewportWidget::pollRegionState()
{
    if (!m_bridge) {
        if (m_hasRegion) {
            m_hasRegion = false;
            update();
        }
        return;
    }
    unsigned int l = 0, t = 0, r = 0, b = 0;
    const bool has = m_bridge->getInteractiveRegion(&l, &t, &r, &b);
    if (has != m_hasRegion || l != m_regionLeft || t != m_regionTop
        || r != m_regionRight || b != m_regionBottom) {
        m_hasRegion = has;
        m_regionLeft = l; m_regionTop = t; m_regionRight = r; m_regionBottom = b;
        update();
    }
}

bool ViewportWidget::gizmoOverlayActive() const
{
    return !m_productionRendering
        && ViewportBridge::categoryForTool(m_activeTool)
           == ViewportBridge::ToolCategory::ObjectTransform;
}

QRect ViewportWidget::imageDrawRect() const
{
    if (m_image.isNull()) return QRect();
    const QSize imgSize = m_image.size();
    if (imgSize.width() <= 0 || imgSize.height() <= 0) return QRect();
    const double scaleX = static_cast<double>(width())  / imgSize.width();
    const double scaleY = static_cast<double>(height()) / imgSize.height();
    const double scale  = std::min(scaleX, scaleY);
    const int drawW = static_cast<int>(imgSize.width()  * scale);
    const int drawH = static_cast<int>(imgSize.height() * scale);
    const int x = (width()  - drawW) / 2;
    const int y = (height() - drawH) / 2;
    return QRect(x, y, drawW, drawH);
}

void ViewportWidget::updateCursorForPosition(const QPointF& pos)
{
    // Tool cursor only over the rendered image area; outside, revert
    // to whatever the parent / window provides (default arrow).
    if (imageDrawRect().contains(pos.toPoint())) {
        setCursor(QCursor(m_toolCursor));
    } else {
        unsetCursor();
    }
}

void ViewportWidget::leaveEvent(QEvent* event)
{
    unsetCursor();
    QWidget::leaveEvent(event);
}

void ViewportWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.fillRect(rect(), palette().window());

    if (m_image.isNull()) {
        p.setPen(palette().placeholderText().color());
        p.drawText(rect(), Qt::AlignCenter, tr("Render to see the scene"));
        return;
    }

    const QRect drawRect = imageDrawRect();
    if (drawRect.isEmpty()) return;
    p.drawImage(drawRect, m_image);

    if (gizmoOverlayActive() && m_bridge) {
        const QSize surface = m_bridge->cameraSurfaceDimensions();
        if (surface.width() > 0 && surface.height() > 0) {
            paintGizmoOverlay(p, drawRect, surface);
        }
    }

    if (m_regionDragging || m_hasRegion) {
        const QSize surface = m_bridge ? m_bridge->cameraSurfaceDimensions() : QSize();
        if (m_regionDragging || (surface.width() > 0 && surface.height() > 0)) {
            paintRegionOverlay(p, drawRect, surface);
        }
    }

    // Navigation axis-ball (Tier 2 §4): always available (not tool-gated),
    // hidden during a production render since the interactive camera state may
    // not match the production frame.
    if (!m_productionRendering && m_sceneEditable && m_bridge) {
        // m_sceneEditable is chat-inclusive: paintNavOverlay reads
        // isFreeFlyActive()/hasHomeView() (controller mutex), which a
        // chat-driven render holds — skip the overlay while render-owned so
        // a repaint can't wedge the Qt main thread.
        paintNavOverlay(p);
    }
}

void ViewportWidget::paintRegionOverlay(QPainter& p, const QRect& drawRect, const QSize& surface)
{
    QRectF boxRect;
    if (m_regionDragging) {
        // A live drag draws directly in widget-local coords -- no
        // surface mapping needed since both endpoints were captured in
        // the same coordinate space.
        const QPointF a = m_regionDragStart;
        const QPointF b = m_regionDragCurrent;
        boxRect = QRectF(QPointF(std::min(a.x(), b.x()), std::min(a.y(), b.y())),
                          QPointF(std::max(a.x(), b.x()), std::max(a.y(), b.y())));
    } else if (m_hasRegion && surface.width() > 0 && surface.height() > 0) {
        const double scaleX = static_cast<double>(drawRect.width())  / surface.width();
        const double scaleY = static_cast<double>(drawRect.height()) / surface.height();
        auto toWidget = [&](double sx, double sy) -> QPointF {
            return QPointF(drawRect.left() + sx * scaleX, drawRect.top() + sy * scaleY);
        };
        // Region bounds are INCLUSIVE full-res film pixels; +1 on the
        // far edge converts to an exclusive draw rect.
        const QPointF tl = toWidget(m_regionLeft, m_regionTop);
        const QPointF br = toWidget(static_cast<double>(m_regionRight) + 1.0,
                                     static_cast<double>(m_regionBottom) + 1.0);
        boxRect = QRectF(tl, br);
    } else {
        m_regionBadgeRect = QRect();
        return;
    }

    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(Theme::warn);
    pen.setStyle(Qt::DashLine);
    pen.setWidthF(1.5);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(boxRect);

    if (!m_regionDragging && m_hasRegion) {
        // "REGION" badge, top-left of the box, click-to-clear (hit-
        // tested against m_regionBadgeRect in mousePressEvent).
        const QString label = tr("REGION");
        const QFont f = Theme::mono(9);
        const QFontMetrics fm(f);
        const int textW = fm.horizontalAdvance(label);
        QRect badge(static_cast<int>(boxRect.left()), static_cast<int>(boxRect.top()) - 20,
                    textW + 14, 16);
        if (badge.top() < 0) {
            // Clamp inside the viewport when the region touches the
            // top edge -- draw the badge just below the box instead.
            badge.moveTop(static_cast<int>(boxRect.top()) + 4);
        }
        m_regionBadgeRect = badge;

        p.setPen(Qt::NoPen);
        p.setBrush(Theme::warn);
        p.drawRoundedRect(badge, 3, 3);
        p.setFont(f);
        p.setPen(QColor(0x0d, 0x0e, 0x10));
        p.drawText(badge, Qt::AlignCenter, label);
    } else {
        m_regionBadgeRect = QRect();
    }
}

void ViewportWidget::paintGizmoOverlay(QPainter& p, const QRect& drawRect,
                                       const QSize& surface)
{
    // Pull the latest handle array from the controller.  The bridge's
    // `refreshGizmoHandles()` runs the C++-side recomputation against
    // the current camera state; `gizmoHandles()` returns a fresh copy.
    m_bridge->refreshGizmoHandles();
    const QVector<ViewportBridge::GizmoHandle> handles = m_bridge->gizmoHandles();
    if (handles.isEmpty()) return;

    // Map a (surface-pixel-space) handle position to widget coords.
    // The controller's `ProjectWorldToScreen_` already outputs
    // widget-Y-DOWN against the stable target dims (`surface`), so we
    // only apply the letter-box scale + offset here.
    const double scaleX = static_cast<double>(drawRect.width())  / surface.width();
    const double scaleY = static_cast<double>(drawRect.height()) / surface.height();
    auto toWidget = [&](double sx, double sy) -> QPointF {
        return QPointF(drawRect.left() + sx * scaleX,
                       drawRect.top()  + sy * scaleY);
    };

    auto axisColor = [&](int axis) -> QColor {
        switch (axis) {
        case 0:  return QColor(220,  60,  60);   // X — red
        case 1:  return QColor( 80, 200,  80);   // Y — green
        case 2:  return QColor( 80, 120, 230);   // Z — blue
        default: return QColor(230, 200,  60);   // screen-aligned — yellow
        }
    };

    const bool   dragActive  = m_bridge->gizmoDragActive();
    const auto   activeKind  = m_bridge->activeGizmoKind();
    const int    activeAxis  = m_bridge->activeGizmoAxis();
    p.setRenderHint(QPainter::Antialiasing, true);

    for (const auto& h : handles) {
        const QPointF c = toWidget(h.screenX, h.screenY);
        const double r = h.screenRadius * std::min(scaleX, scaleY);
        const QColor color = axisColor(h.axis);
        const bool   isActive = dragActive
                             && activeKind == h.kind
                             && activeAxis == h.axis;
        const QColor strokeC = isActive ? QColor(255, 255, 255) : color;
        QPen stroke(strokeC);
        stroke.setWidthF(isActive ? 2.5 : 1.5);

        using K = ViewportBridge::GizmoKind;
        switch (h.kind) {
        case K::AxisArrow:
        case K::AxisScaleHandle: {
            QColor fill = color;
            fill.setAlphaF(0.85f);
            p.setBrush(fill);
            p.setPen(stroke);
            p.drawEllipse(c, r, r);
            break;
        }
        case K::AxisPlane: {
            const double s = r * 1.4;
            QColor fill = color;
            fill.setAlphaF(0.40f);
            p.setBrush(fill);
            p.setPen(stroke);
            p.drawRect(QRectF(c.x() - s, c.y() - s, 2 * s, 2 * s));
            break;
        }
        case K::ScreenCenter:
        case K::UniformScaleCube: {
            QColor fill = color;
            fill.setAlphaF(0.30f);
            p.setBrush(fill);
            p.setPen(stroke);
            p.drawEllipse(c, r, r);
            break;
        }
        case K::AxisRing:
        case K::ScreenRing: {
            p.setBrush(Qt::NoBrush);
            QColor ringC = strokeC;
            ringC.setAlphaF(isActive ? 1.0f : 0.8f);
            QPen ringPen(ringC);
            ringPen.setWidthF(isActive ? 3.0 : 2.0);
            p.setPen(ringPen);
            p.drawEllipse(c, r, r);
            break;
        }
        }
    }
}

QPointF ViewportWidget::surfacePoint(const QPointF& pos) const
{
    // Map logical widget coords to image-pixel space — the same coord
    // system the rasterizer / camera use internally (0..GetWidth() ×
    // 0..GetHeight()).  Two adjustments:
    //
    //   1. Aspect-fit offset: the image is drawn centred in the
    //      widget with letterbox / pillarbox margins; subtract the
    //      draw-rect origin so widget-relative coords become
    //      image-relative.
    //   2. Pixel-density rescale: divide by drawRect.size (widget
    //      points) and multiply by the camera's STABLE full-res
    //      dimensions (NOT m_image.size which shrinks during
    //      preview-scale subsampling — that path makes mLastPx and
    //      the next event live in mismatched coord spaces and
    //      produces 4×–32× pan/orbit jumps when the scale state
    //      machine steps).  cameraSurfaceDimensions reads from
    //      SceneEditController::mFullResW/H which are refreshed
    //      before every render pass's dim swap, so the value is
    //      stable across the swap window.  Fall back to m_image's
    //      size only when the bridge isn't wired (test mode etc.)
    //      to keep the legacy behaviour as a graceful default.
    //
    // The ratio handles HiDPI implicitly — devicePixelRatioF doesn't
    // need to enter the math because the drawRect is in widget points
    // and the surface dims are in rasterizer pixels.  Coords may fall
    // outside [0, surface] if the user drags past the edge; the
    // controller treats that as "no hit" for picking and as a normal
    // delta for motion tools.
    const QRect drawRect = imageDrawRect();
    if (drawRect.isEmpty()) {
        return pos;   // best-effort fallback; controller may still cope
    }

    QSize surface = m_bridge ? m_bridge->cameraSurfaceDimensions() : QSize();
    if (surface.width() <= 0 || surface.height() <= 0) {
        if (m_image.isNull()) return pos;
        surface = m_image.size();
    }
    if (surface.width() <= 0 || surface.height() <= 0) return pos;

    const double nx = (pos.x() - drawRect.left()) / drawRect.width();
    const double ny = (pos.y() - drawRect.top())  / drawRect.height();
    return QPointF(nx * surface.width(), ny * surface.height());
}

// -------- Navigation axis-ball (Tier 2 §4) --------

static constexpr double kNavInset  = 16.0;   // ball inset from the top-right corner
static constexpr double kNavBallR  = 30.0;   // ball radius (widget px)
static constexpr double kNavNubR   = 9.0;    // nub radius
static constexpr double kNavBtnW   = 24.0;   // control-button width
static constexpr double kNavBtnH   = 20.0;   // control-button height
static constexpr double kNavBtnGap = 6.0;

QPointF ViewportWidget::navBallCenter() const
{
    return QPointF(width() - kNavInset - kNavBallR, kNavInset + kNavBallR);
}

void ViewportWidget::navControlRects(QRectF& outHome, QRectF& outSet,
                                     QRectF& outStamp, QRectF& outExit) const
{
    // A centered row just below the ball: Home, Set-home, and (only while
    // free-flying) Stamp-to-camera + Back-to-camera.
    const QPointF c = navBallCenter();
    const double rowY = c.y() + kNavBallR + kNavNubR + 6.0;
    const bool freeFly = m_bridge && m_bridge->isFreeFlyActive();
    const int nButtons = freeFly ? 4 : 2;
    const double totalW = nButtons * kNavBtnW + (nButtons - 1) * kNavBtnGap;
    double x = c.x() - totalW / 2.0;
    outHome  = QRectF(x, rowY, kNavBtnW, kNavBtnH); x += kNavBtnW + kNavBtnGap;
    outSet   = QRectF(x, rowY, kNavBtnW, kNavBtnH); x += kNavBtnW + kNavBtnGap;
    outStamp = freeFly ? QRectF(x, rowY, kNavBtnW, kNavBtnH) : QRectF();
    if (freeFly) x += kNavBtnW + kNavBtnGap;
    outExit  = freeFly ? QRectF(x, rowY, kNavBtnW, kNavBtnH) : QRectF();
}

void ViewportWidget::paintNavOverlay(QPainter& p)
{
    if (!m_bridge) return;
    const QPointF c = navBallCenter();
    if (!m_bridge->refreshNavGizmo(c.x(), c.y(), kNavBallR, kNavNubR)) {
        return;   // no supported (pinhole) camera -> draw nothing
    }
    QVector<ViewportBridge::NavNub> nubs = m_bridge->navGizmoNubs();

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);

    // Ball backdrop.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 56));
    p.drawEllipse(c, kNavBallR, kNavBallR);

    auto axisColor = [](int axis) -> QColor {
        switch (axis) {
        case 0:  return QColor(220, 60, 60);
        case 1:  return QColor(80, 200, 80);
        case 2:  return QColor(80, 120, 230);
        default: return QColor(220, 200, 80);
        }
    };

    // Back-facing nubs first so front-facing ones draw on top.
    std::stable_sort(nubs.begin(), nubs.end(),
        [](const ViewportBridge::NavNub& a, const ViewportBridge::NavNub& b) {
            return (a.facing ? 1 : 0) < (b.facing ? 1 : 0);
        });

    static const char kLetters[3] = { 'X', 'Y', 'Z' };
    for (const auto& nub : nubs) {
        QColor col = axisColor(nub.axis);
        col.setAlpha(nub.facing ? 255 : 110);
        const QPointF nc(nub.screenX, nub.screenY);
        const double r = nub.screenRadius;
        if (nub.negative) {
            p.setBrush(QColor(0, 0, 0, nub.facing ? 90 : 40));
            p.setPen(QPen(col, 2));
            p.drawEllipse(nc, r, r);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            p.drawEllipse(nc, r, r);
            const int a = (nub.axis >= 0 && nub.axis < 3) ? nub.axis : 0;
            p.setPen(QColor(255, 255, 255, nub.facing ? 240 : 130));
            QFont f = p.font(); f.setBold(true); f.setPointSizeF(r * 0.9); p.setFont(f);
            p.drawText(QRectF(nc.x() - r, nc.y() - r, r * 2, r * 2),
                       Qt::AlignCenter, QString(QChar(kLetters[a])));
        }
    }

    // Control buttons.
    QRectF homeR, setR, stampR, exitR;
    navControlRects(homeR, setR, stampR, exitR);
    const bool homeEnabled = m_bridge->hasHomeView();
    auto drawBtn = [&](const QRectF& rc, const QString& label, bool enabled) {
        if (rc.isNull()) return;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, enabled ? 120 : 60));
        p.drawRoundedRect(rc, 4, 4);
        p.setPen(QColor(255, 255, 255, enabled ? 230 : 90));
        QFont f = p.font(); f.setBold(true); f.setPointSizeF(10); p.setFont(f);
        p.drawText(rc, Qt::AlignCenter, label);
    };
    drawBtn(homeR, QStringLiteral("H"), homeEnabled);   // go Home
    drawBtn(setR,  QStringLiteral("S"), true);           // Set home
    if (!stampR.isNull()) drawBtn(stampR, QStringLiteral("+"), true);  // stamp view to new camera
    if (!exitR.isNull())  drawBtn(exitR,  QStringLiteral("C"), true);  // back to scene Camera

    p.restore();
}

bool ViewportWidget::handleNavClick(const QPointF& widgetPos)
{
    if (!m_bridge) return false;

    // Gate on the SAME condition as paint: the whole overlay (ball + nubs +
    // control buttons) is drawn only when the nav gizmo lays out (a supported
    // pinhole interactive camera).  When it doesn't, nothing is visible — so
    // nothing must consume clicks (no invisible dead zone in the corner).
    // This also re-lays-out at the current ball geometry so the nub hit-test
    // below matches what was painted.
    const QPointF c = navBallCenter();
    if (!m_bridge->refreshNavGizmo(c.x(), c.y(), kNavBallR, kNavNubR)) return false;

    // Control buttons first (they sit below the ball, overlapping no nub).
    QRectF homeR, setR, stampR, exitR;
    navControlRects(homeR, setR, stampR, exitR);
    if (homeR.contains(widgetPos)) {
        if (m_bridge->hasHomeView()) { m_bridge->goToHomeView(); update(); }
        return true;   // consume even when disabled — it's our visible control area
    }
    if (setR.contains(widgetPos)) { m_bridge->setHomeView(); update(); return true; }
    if (!stampR.isNull() && stampR.contains(widgetPos)) {
        // Auto-named + dedup-suffixed by the core; the new camera becomes
        // active. The outliner picks it up via its scene-epoch poll.
        m_bridge->stampViewToNewCamera(QStringLiteral("view_camera"));
        update();
        return true;
    }
    if (!exitR.isNull() && exitR.contains(widgetPos)) { m_bridge->exitFreeFly(); update(); return true; }

    // Nub hit-test → snap the view.
    const int idx = m_bridge->navGizmoNubAt(widgetPos.x(), widgetPos.y());
    if (idx < 0) return false;
    const QVector<ViewportBridge::NavNub> nubs = m_bridge->navGizmoNubs();
    if (idx >= nubs.size()) return false;
    const ViewportBridge::NavNub& n = nubs[idx];
    m_bridge->snapViewToAxis(n.axis, n.negative);
    update();
    return true;
}

void ViewportWidget::mousePressEvent(QMouseEvent* event)
{
    // Every press starts a NEW physical gesture -- any suppress flag
    // left over from an earlier interrupted gesture (e.g. a lost
    // mouseReleaseEvent) is stale by definition.  Clearing here --
    // rather than only on the matching release -- bounds the flag's
    // lifetime to at most one gesture.  Mirrors ViewportView.swift's
    // pointer-down handler clearing `suppressPointerUntilUp`.
    m_suppressPointerUntilRelease = false;

    // RISE UI redesign (A4 region refinement): while armed, a press
    // starts a region drag instead of forwarding to the bridge --
    // mirrors the Mac's suppressPointerUntilUp guard.
    if (m_regionArmed && event->button() == Qt::LeftButton) {
        m_regionDragging = true;
        m_regionDragStart = event->position();
        m_regionDragCurrent = event->position();
        update();
        event->accept();
        return;
    }

    // A click on the persistent region badge clears the region --
    // consumes the click rather than also forwarding it as a pick.
    if (m_sceneEditable && !m_regionArmed && m_hasRegion && event->button() == Qt::LeftButton
        && m_regionBadgeRect.contains(event->pos())) {
        // m_sceneEditable gate: clearInteractiveRegion -> KickRender takes the
        // controller mutex a chat/production render holds for its duration.
        if (m_bridge) m_bridge->clearInteractiveRegion();
        m_hasRegion = false;
        update();
        event->accept();
        return;
    }

    // Navigation axis-ball: a nub click snaps the view; the labeled controls
    // drive Home / free-fly.  Consumes the click so it isn't also forwarded as
    // a scene pick.  Gated off during production render (the ball isn't drawn).
    if (!m_productionRendering && m_sceneEditable && event->button() == Qt::LeftButton
        && handleNavClick(event->position())) {
        event->accept();
        return;
    }

    // m_sceneEditable is chat-inclusive: pointerDown/Move/Up all take the
    // controller commit mutex, which a chat-driven render holds for its whole
    // duration — gate them so a viewport gesture during a chat render can't
    // wedge the Qt main thread.  A chat render that begins mid-gesture drops
    // the matching Up (same as an already-handled lost release; the next
    // pointerDown re-establishes clean state).
    if (m_bridge && m_sceneEditable) {
        const QPointF p = surfacePoint(event->position());
        m_bridge->pointerDown(p.x(), p.y());
    }
    event->accept();
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* event)
{
    // Tracking mouse — swap the cursor based on whether we're over
    // the rendered image area or the empty surround.
    updateCursorForPosition(event->position());

    if (m_regionDragging) {
        m_regionDragCurrent = event->position();
        update();
        event->accept();
        return;
    }

    if (m_suppressPointerUntilRelease) {
        // Trailing leg of a gesture Escape/disarm cancelled mid-drag
        // (see cancelRegionDrag()'s doc) -- swallow it rather than
        // forward a pointerMove with no matching pointerDown.
        event->accept();
        return;
    }

    if (m_bridge && m_sceneEditable && (event->buttons() & Qt::LeftButton)) {
        const QPointF p = surfacePoint(event->position());
        m_bridge->pointerMove(p.x(), p.y());
    }
    event->accept();
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_regionDragging) {
        m_regionDragging = false;
        m_regionArmed = false;   // consumed -- one drag per arm

        // Convert the two widget-local drag endpoints to full-res film
        // pixels via the SAME aspect-fit mapping surfacePoint() uses
        // for ordinary pointer events, then commit an INCLUSIVE box
        // (ViewportBridge::setInteractiveRegion's documented contract).
        const QPointF startSurface = surfacePoint(m_regionDragStart);
        const QPointF endSurface   = surfacePoint(event->position());

        int sx0 = static_cast<int>(std::floor(std::min(startSurface.x(), endSurface.x())));
        int sy0 = static_cast<int>(std::floor(std::min(startSurface.y(), endSurface.y())));
        int sx1 = static_cast<int>(std::ceil(std::max(startSurface.x(), endSurface.x()))) - 1;
        int sy1 = static_cast<int>(std::ceil(std::max(startSurface.y(), endSurface.y()))) - 1;

        const QSize surface = m_bridge ? m_bridge->cameraSurfaceDimensions() : QSize();
        if (surface.width() > 0 && surface.height() > 0) {
            sx0 = std::clamp(sx0, 0, surface.width() - 1);
            sy0 = std::clamp(sy0, 0, surface.height() - 1);
            sx1 = std::clamp(sx1, 0, surface.width() - 1);
            sy1 = std::clamp(sy1, 0, surface.height() - 1);
        } else {
            sx0 = std::max(0, sx0);
            sy0 = std::max(0, sy0);
            sx1 = std::max(sx0, sx1);
            sy1 = std::max(sy0, sy1);
        }

        // Reject a degenerate (near-zero-size) drag -- a stray click
        // while armed shouldn't silently set a 1x1 region.
        if (m_bridge && m_sceneEditable && (sx1 - sx0) >= 2 && (sy1 - sy0) >= 2) {
            // m_sceneEditable gate: setInteractiveRegion -> KickRender takes the
            // controller mutex a chat/production render holds (a chat render can
            // begin between this drag's press and release).
            m_bridge->setInteractiveRegion(
                static_cast<unsigned int>(sx0), static_cast<unsigned int>(sy0),
                static_cast<unsigned int>(sx1), static_cast<unsigned int>(sy1));
            pollRegionState();
        }
        update();
        event->accept();
        return;
    }

    if (m_suppressPointerUntilRelease) {
        // Last leg of a gesture Escape/disarm cancelled mid-drag (see
        // cancelRegionDrag()'s doc) -- swallow it and clear the flag so
        // the NEXT physical gesture starts clean.  (mousePressEvent
        // also clears it defensively, but doing it here too ends the
        // flag's lifetime at the natural end of the gesture it guards,
        // rather than leaving it lit until some future press.)
        m_suppressPointerUntilRelease = false;
        event->accept();
        return;
    }

    if (m_bridge && m_sceneEditable) {
        const QPointF p = surfacePoint(event->position());
        m_bridge->pointerUp(p.x(), p.y());
    }
    event->accept();
}

void ViewportWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && (m_regionArmed || m_regionDragging)) {
        cancelRegionDrag();
        m_regionArmed = false;
        emit regionArmCancelled();
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
