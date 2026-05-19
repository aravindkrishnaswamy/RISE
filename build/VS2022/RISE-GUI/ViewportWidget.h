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

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QPointF surfacePoint(const QPointF& pos) const;
    QRect   imageDrawRect() const;
    void    updateCursorForPosition(const QPointF& pos);
    void    paintGizmoOverlay(QPainter& p, const QRect& drawRect, const QSize& surface);
    bool    gizmoOverlayActive() const;

    ViewportBridge*  m_bridge = nullptr;
    QImage           m_image;
    Qt::CursorShape  m_toolCursor = Qt::ArrowCursor;
    ViewportTool     m_activeTool = ViewportTool::Select;
    bool             m_productionRendering = false;
};

#endif // VIEWPORTWIDGET_H
