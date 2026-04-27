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
