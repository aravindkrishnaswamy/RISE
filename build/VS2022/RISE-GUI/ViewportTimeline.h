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
class QToolButton;
class QTimer;

class ViewportTimeline : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportTimeline(QWidget* parent = nullptr);

    void setRange(double minT, double maxT);
    double currentTime() const { return m_time; }

    // Number of frames in the active animation.  Drives the per-tick
    // step of the Play preview: dt = (maxT - minT) / max(frames-1, 1).
    // Defaults to 30 until set.
    void setAnimationFrameCount(unsigned int numFrames);

    // Halt the Play preview if it is running.  Public so MainWindow can
    // stop a running preview-play QTimer BEFORE a production render
    // begins (disabling the widget does NOT stop a live QTimer).
    // Idempotent / safe to call when not playing.
    void stopPlayback();

signals:
    void scrubBegin();
    void scrubEnd();
    void timeChanged(double t);

private slots:
    void onSliderPressed();
    void onSliderReleased();
    void onSliderMoved(int sliderValue);

    // Play/Stop toggle (QToolButton::toggled) and the per-tick advance.
    void onPlayToggled(bool play);
    void onPlayTick();

private:
    void updateLabels();

    // Set the slider + m_time to a given scene time WITHOUT emitting
    // timeChanged (used by the Play tick, which emits timeChanged
    // itself after positioning the slider — emitting from the slider
    // setter too would double-fire).
    void setTimeValue(double t);

    QSlider*     m_slider = nullptr;
    QLabel*      m_currentLabel = nullptr;
    QLabel*      m_maxLabel = nullptr;
    QToolButton* m_playButton = nullptr;
    QTimer*      m_playTimer = nullptr;
    double       m_minT = 0.0;
    double       m_maxT = 5.0;
    double       m_time = 0.0;
    unsigned int m_numFrames = 30;
    bool         m_scrubbing = false;
    bool         m_playing = false;
};

#endif // VIEWPORTTIMELINE_H
