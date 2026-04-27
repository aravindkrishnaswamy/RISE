//////////////////////////////////////////////////////////////////////
//
//  ViewportTimeline.h - Time-scrubber widget.  Shown only when the
//    loaded scene has any keyframed elements.
//
//////////////////////////////////////////////////////////////////////

#ifndef VIEWPORTTIMELINE_H
#define VIEWPORTTIMELINE_H

#include <QWidget>

class QSlider;
class QLabel;

class ViewportTimeline : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportTimeline(QWidget* parent = nullptr);

    void setRange(double minT, double maxT);
    double currentTime() const { return m_time; }

signals:
    void scrubBegin();
    void scrubEnd();
    void timeChanged(double t);

private slots:
    void onSliderPressed();
    void onSliderReleased();
    void onSliderMoved(int sliderValue);

private:
    void updateLabels();

    QSlider* m_slider = nullptr;
    QLabel*  m_currentLabel = nullptr;
    QLabel*  m_maxLabel = nullptr;
    double   m_minT = 0.0;
    double   m_maxT = 5.0;
    double   m_time = 0.0;
    bool     m_scrubbing = false;
};

#endif // VIEWPORTTIMELINE_H
