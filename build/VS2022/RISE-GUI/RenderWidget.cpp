//////////////////////////////////////////////////////////////////////
//
//  RenderWidget.cpp - Image display widget with progressive updates.
//
//  Ported from the Mac app's RenderImageView.swift.
//
//////////////////////////////////////////////////////////////////////

#include "RenderWidget.h"

#include <QPainter>
#include <QVBoxLayout>
#include <QResizeEvent>
#include <QStyle>

RenderWidget::RenderWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(200, 150);

    // Loading overlay (hidden by default)
    m_loadingOverlay = new QWidget(this);
    m_loadingOverlay->setFixedSize(250, 120);
    m_loadingOverlay->setStyleSheet(
        "background-color: rgba(0, 0, 0, 160); border-radius: 12px;");
    m_loadingOverlay->hide();

    auto* overlayLayout = new QVBoxLayout(m_loadingOverlay);
    overlayLayout->setAlignment(Qt::AlignCenter);

    m_loadingLabel = new QLabel("Loading scene...", m_loadingOverlay);
    m_loadingLabel->setStyleSheet("color: white; font-size: 14px;");
    m_loadingLabel->setAlignment(Qt::AlignCenter);
    overlayLayout->addWidget(m_loadingLabel);

    m_loadingProgress = new QProgressBar(m_loadingOverlay);
    m_loadingProgress->setFixedWidth(200);
    m_loadingProgress->setRange(0, 0); // Indeterminate
    m_loadingProgress->setTextVisible(false);
    overlayLayout->addWidget(m_loadingProgress, 0, Qt::AlignCenter);

    m_loadingPercent = new QLabel("", m_loadingOverlay);
    m_loadingPercent->setStyleSheet("color: white; font-family: monospace;");
    m_loadingPercent->setAlignment(Qt::AlignCenter);
    m_loadingPercent->hide();
    overlayLayout->addWidget(m_loadingPercent);
}

void RenderWidget::setRenderState(RenderEngine::State state)
{
    m_state = state;
    updateOverlay();
    update();
}

void RenderWidget::updateImage(const QImage& image)
{
    if (image.isNull()) {
        m_pixmap = QPixmap();
        m_hasImage = false;
    } else {
        m_pixmap = QPixmap::fromImage(image);
        m_hasImage = true;
    }
    update();
}

void RenderWidget::setProgress(double fraction)
{
    if (m_loadingProgress->maximum() == 0 && fraction > 0) {
        m_loadingProgress->setRange(0, 1000);
    }
    m_loadingProgress->setValue(static_cast<int>(fraction * 1000));
    m_loadingPercent->setText(QString("%1%").arg(fraction * 100, 0, 'f', 1));
    m_loadingPercent->setVisible(fraction > 0);
}

void RenderWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Mid));

    if (m_hasImage && !m_pixmap.isNull()) {
        // Nearest-neighbor interpolation (matching Mac app's .interpolation(.none))
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

        // Aspect-fit scaling
        QSize pixSize = m_pixmap.size();
        QSize widgetSize = size();
        QSize scaled = pixSize.scaled(widgetSize, Qt::KeepAspectRatio);

        int x = (widgetSize.width() - scaled.width()) / 2;
        int y = (widgetSize.height() - scaled.height()) / 2;

        painter.drawPixmap(x, y, scaled.width(), scaled.height(), m_pixmap);
    } else if (m_state != RenderEngine::Loading) {
        // Placeholder
        painter.setPen(palette().color(QPalette::PlaceholderText));

        QFont font = painter.font();
        font.setPointSize(14);
        painter.setFont(font);

        QString text;
        if (m_state == RenderEngine::Idle) {
            text = "Open a .RISEscene file to begin";
        } else if (m_state == RenderEngine::SceneLoaded) {
            text = "Press Render to start";
        }

        if (!text.isEmpty()) {
            painter.drawText(rect(), Qt::AlignCenter, text);
        }
    }
}

void RenderWidget::updateOverlay()
{
    m_loadingOverlay->setVisible(m_state == RenderEngine::Loading);
    positionOverlay();
}

void RenderWidget::positionOverlay()
{
    if (m_loadingOverlay->isVisible()) {
        int x = (width() - m_loadingOverlay->width()) / 2;
        int y = (height() - m_loadingOverlay->height()) / 2;
        m_loadingOverlay->move(x, y);
    }
}

void RenderWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    positionOverlay();
}
