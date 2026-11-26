//////////////////////////////////////////////////////////////////////
//
//  RenderWidget.h - Displays the rendered image with aspect-fit
//  scaling and nearest-neighbor interpolation, plus loading overlay
//  and placeholder text.
//
//////////////////////////////////////////////////////////////////////

#ifndef RENDERWIDGET_H
#define RENDERWIDGET_H

#include <QWidget>
#include <QImage>
#include <QPixmap>
#include <QProgressBar>
#include <QLabel>

#include "RenderEngine.h"

class RenderWidget : public QWidget
{
    Q_OBJECT

public:
    explicit RenderWidget(QWidget* parent = nullptr);

    void setRenderState(RenderEngine::State state);

public slots:
    void updateImage(const QImage& image);
    void setProgress(double fraction);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPixmap m_pixmap;
    RenderEngine::State m_state = RenderEngine::Idle;
    bool m_hasImage = false;

    // Loading overlay widgets
    QWidget* m_loadingOverlay = nullptr;
    QProgressBar* m_loadingProgress = nullptr;
    QLabel* m_loadingLabel = nullptr;
    QLabel* m_loadingPercent = nullptr;

    void updateOverlay();
    void positionOverlay();
    void resizeEvent(QResizeEvent* event) override;
};

#endif // RENDERWIDGET_H
