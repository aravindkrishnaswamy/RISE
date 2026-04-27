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
    /// active tool.  Wired to ViewportToolbar::toolChanged.
    void setActiveTool(ViewportTool t);

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

    ViewportBridge*  m_bridge = nullptr;
    QImage           m_image;
    Qt::CursorShape  m_toolCursor = Qt::ArrowCursor;
};

#endif // VIEWPORTWIDGET_H
