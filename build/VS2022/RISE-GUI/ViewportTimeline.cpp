//////////////////////////////////////////////////////////////////////
//
//  ViewportTimeline.cpp
//
//////////////////////////////////////////////////////////////////////

#include "ViewportTimeline.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>

ViewportTimeline::ViewportTimeline(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(8);

    m_currentLabel = new QLabel(this);
    m_currentLabel->setMinimumWidth(60);
    m_currentLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, 1000);   // virtual ticks; we map to [m_minT, m_maxT]
    m_slider->setValue(0);

    m_maxLabel = new QLabel(this);
    m_maxLabel->setMinimumWidth(60);
    m_maxLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    layout->addWidget(m_currentLabel);
    layout->addWidget(m_slider, 1);
    layout->addWidget(m_maxLabel);

    connect(m_slider, &QSlider::sliderPressed,  this, &ViewportTimeline::onSliderPressed);
    connect(m_slider, &QSlider::sliderReleased, this, &ViewportTimeline::onSliderReleased);
    connect(m_slider, &QSlider::sliderMoved,    this, &ViewportTimeline::onSliderMoved);

    updateLabels();
}

void ViewportTimeline::setRange(double minT, double maxT)
{
    m_minT = minT;
    m_maxT = maxT;
    updateLabels();
}

void ViewportTimeline::onSliderPressed()
{
    m_scrubbing = true;
    emit scrubBegin();
}

void ViewportTimeline::onSliderReleased()
{
    if (m_scrubbing) {
        m_scrubbing = false;
        emit scrubEnd();
    }
}

void ViewportTimeline::onSliderMoved(int sliderValue)
{
    const double frac = sliderValue / static_cast<double>(m_slider->maximum());
    m_time = m_minT + frac * (m_maxT - m_minT);
    updateLabels();
    emit timeChanged(m_time);
}

void ViewportTimeline::updateLabels()
{
    m_currentLabel->setText(QString::number(m_time, 'f', 2) + "s");
    m_maxLabel->setText(QString::number(m_maxT, 'f', 2) + "s");
}
