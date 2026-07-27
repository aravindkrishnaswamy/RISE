//////////////////////////////////////////////////////////////////////
//
//  ViewportTimeline.h - Time-scrubber widget.  Shown whenever the live
//    scene has keyframed elements, including timelines added after load.
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

    /// Replace the live animation range using the controller's canonical
    /// time (the widget can be stale after Undo/Redo).  A change stops
    /// playback, reprojects an in-range playhead, and moves an out-of-range
    /// playhead through the normal bracketed scrub signals.
    void setRange(double minT, double maxT, double canonicalTime);
    double currentTime() const { return m_time; }

    // Number of frames in the active animation.  Drives the per-tick
    // step of the Play preview: dt = (maxT - minT) / max(frames-1, 1).
    // Defaults to 30 until set.  A live count change stops playback so one
    // run never spans two different active-animation option tuples.
    void setAnimationFrameCount(unsigned int numFrames);

    // Halt the Play preview if it is running.  Public so MainWindow can
    // stop a running preview-play QTimer BEFORE a production render
    // begins (disabling the widget does NOT stop a live QTimer).
    // Idempotent / safe to call when not playing.
    void stopPlayback();

public slots:
    /// Mirrors MainWindow::updateMenuActionStates' gate on
    /// m_renderAnimAction (canRender && hasAnimation) -- the "Render
    /// movie…" chip must never be clickable when the menu equivalent
    /// isn't either.
    void setRenderMovieEnabled(bool enabled);

protected:
    // LIVE THEME-SWITCH CONTRACT (Theme.h): every widget with token-
    // dependent styling hooks QEvent::PaletteChange here and calls its
    // own restyleTheme(). See MainWindow::changeEvent for the reference
    // implementation this mirrors.
    void changeEvent(QEvent* e) override;

signals:
    void scrubBegin();
    void scrubEnd();
    void timeChanged(double t);

    /// "Render movie…" chip clicked -- MainWindow connects this to
    /// onRenderAnimation() (the SAME slot the Render > Render Animation
    /// menu item drives), so there is exactly one render-animation code
    /// path regardless of which affordance the user clicked.
    void renderMovieClicked();

private slots:
    void onSliderPressed();
    void onSliderReleased();
    void onSliderMoved(int sliderValue);

    // Play/Stop toggle (QToolButton::toggled) and the per-tick advance.
    void onPlayToggled(bool play);
    void onPlayTick();

    void onRewindClicked();
    void onToEndClicked();

private:
    // LIVE THEME-SWITCH CONTRACT (Theme.h): re-applies every one of this
    // widget's own token-dependent styling sites -- the palette fill,
    // the widget's own border QSS, the rewind/play/skip icon tints, the
    // play button's/slider's/render-movie chip's QSS, and the current/
    // max time label colors -- from the CURRENT Theme:: token values.
    // Called once at the end of the constructor and again from
    // changeEvent() on QEvent::PaletteChange. Idempotent, creates no
    // widgets.
    void restyleTheme();

    // LIVE THEME-SWITCH CONTRACT point 4 (Theme.h) -- re-entrancy guard.
    // See MainWindow.h for the full rationale; uniform across every
    // changeEvent()-overriding class.
    bool m_themeReady = false;
    int  m_themeEpochSeen = -1;

    void updateLabels();

    // Set the slider + m_time to a given scene time WITHOUT emitting
    // timeChanged (used by the Play tick, which emits timeChanged
    // itself after positioning the slider — emitting from the slider
    // setter too would double-fire).
    void setTimeValue(double t);

    // Jump directly to `t`, bracketed as a single scrub (one undo
    // entry) -- shared by the ⏮ rewind and ⏭ to-end transport buttons.
    void jumpToTime(double t);

    static QString formatTime(double seconds);

    QToolButton* m_rewindButton = nullptr;
    QToolButton* m_toEndButton  = nullptr;
    QSlider*     m_slider = nullptr;
    QLabel*      m_currentLabel = nullptr;
    QLabel*      m_maxLabel = nullptr;
    QToolButton* m_playButton = nullptr;
    QToolButton* m_renderMovieBtn = nullptr;
    QTimer*      m_playTimer = nullptr;
    double       m_minT = 0.0;
    double       m_maxT = 5.0;
    double       m_time = 0.0;
    unsigned int m_numFrames = 30;
    bool         m_scrubbing = false;
    bool         m_playing = false;
};

#endif // VIEWPORTTIMELINE_H
