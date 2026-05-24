//////////////////////////////////////////////////////////////////////
//
//  ViewportWidget.cpp
//
//////////////////////////////////////////////////////////////////////

#include "ViewportWidget.h"
#include "ViewportBridge.h"

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QWindow>
#include <QCursor>

ViewportWidget::ViewportWidget(ViewportBridge* bridge, QWidget* parent)
    : QWidget(parent)
    , m_bridge(bridge)
{
    setAutoFillBackground(true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);
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

void ViewportWidget::mousePressEvent(QMouseEvent* event)
{
    if (m_bridge) {
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

    if (m_bridge && (event->buttons() & Qt::LeftButton)) {
        const QPointF p = surfacePoint(event->position());
        m_bridge->pointerMove(p.x(), p.y());
    }
    event->accept();
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_bridge) {
        const QPointF p = surfacePoint(event->position());
        m_bridge->pointerUp(p.x(), p.y());
    }
    event->accept();
}
