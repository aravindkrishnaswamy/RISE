//////////////////////////////////////////////////////////////////////
//
//  ViewportTimeline.cpp
//
//////////////////////////////////////////////////////////////////////

#include "ViewportTimeline.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <QTimer>
#include <QStyle>
#include <QSignalBlocker>

ViewportTimeline::ViewportTimeline(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(8);

    // Play/Stop toggle.  Checkable — checked = playing.  Uses the
    // platform style's media-play icon so the affordance reads as a
    // transport control rather than a generic button.
    m_playButton = new QToolButton(this);
    m_playButton->setCheckable(true);
    m_playButton->setToolTip("Play the active animation (loops until stopped)");
    m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));

    m_currentLabel = new QLabel(this);
    m_currentLabel->setMinimumWidth(60);
    m_currentLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, 1000);   // virtual ticks; we map to [m_minT, m_maxT]
    m_slider->setValue(0);

    m_maxLabel = new QLabel(this);
    m_maxLabel->setMinimumWidth(60);
    m_maxLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    layout->addWidget(m_playButton);
    layout->addWidget(m_currentLabel);
    layout->addWidget(m_slider, 1);
    layout->addWidget(m_maxLabel);

    // Playback pacing timer.  Coarse type is fine for a ~30 fps
    // preview — we don't need sub-ms accuracy and CoarseTimer is
    // cheaper / coalesces better.  Pacing is strictly timer-driven;
    // each tick advances regardless of whether the previous frame has
    // finished rendering (the preview renderer drops stale frames).
    m_playTimer = new QTimer(this);
    m_playTimer->setTimerType(Qt::CoarseTimer);
    m_playTimer->setInterval(33);   // ~30 fps

    connect(m_slider, &QSlider::sliderPressed,  this, &ViewportTimeline::onSliderPressed);
    connect(m_slider, &QSlider::sliderReleased, this, &ViewportTimeline::onSliderReleased);
    connect(m_slider, &QSlider::sliderMoved,    this, &ViewportTimeline::onSliderMoved);
    connect(m_playButton, &QToolButton::toggled, this, &ViewportTimeline::onPlayToggled);
    connect(m_playTimer,  &QTimer::timeout,      this, &ViewportTimeline::onPlayTick);

    updateLabels();
}

void ViewportTimeline::setRange(double minT, double maxT)
{
    m_minT = minT;
    m_maxT = maxT;
    updateLabels();
}

void ViewportTimeline::setAnimationFrameCount(unsigned int numFrames)
{
    m_numFrames = numFrames;
}

void ViewportTimeline::onSliderPressed()
{
    // Manual scrub interrupts playback — un-checking the Play button
    // routes through onPlayToggled, which stops the timer and closes
    // the play run's scrub bracket cleanly before we open the manual
    // drag's own bracket below.
    if (m_playing && m_playButton) {
        m_playButton->setChecked(false);
    }
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

void ViewportTimeline::setTimeValue(double t)
{
    // Position the slider + labels for time `t` WITHOUT emitting
    // timeChanged.  The slider's own valueChanged would otherwise fire
    // on setValue, but we only listen to sliderMoved (user-drag) — so a
    // programmatic setValue here is already non-emitting w.r.t. our
    // scrub wire.  Block signals anyway to be explicit and future-proof.
    m_time = t;
    const double span = (m_maxT - m_minT);
    const double frac = (span > 0.0) ? (t - m_minT) / span : 0.0;
    const int sliderVal = static_cast<int>(frac * m_slider->maximum() + 0.5);
    {
        QSignalBlocker blocker(m_slider);
        m_slider->setValue(qBound(m_slider->minimum(), sliderVal, m_slider->maximum()));
    }
    updateLabels();
}

void ViewportTimeline::onPlayToggled(bool play)
{
    // Ignore redundant toggles so we never open/close an unbalanced
    // scrub bracket (e.g. setChecked() to the already-current state).
    if (play == m_playing) return;

    // A zero-length range (active animation with time_start == time_end)
    // would make dt == 0 and the playhead never advance — reject the start
    // so Play can't get visually "stuck".  Revert the button; the resulting
    // re-entrant onPlayToggled(false) is a no-op (play == m_playing).
    if (play && m_maxT <= m_minT) {
        if (m_playButton) m_playButton->setChecked(false);
        return;
    }

    if (play) {
        m_playing = true;
        if (m_playButton) {
            m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
        }
        // Start the play run at time_start so the loop is deterministic
        // regardless of where the slider happened to be.  Bracket the
        // whole run as a single scrub (one undo entry) — matches a
        // manual drag, which brackets press→release.
        emit scrubBegin();
        setTimeValue(m_minT);
        emit timeChanged(m_time);
        m_playTimer->start();
    } else {
        // Stop: halt the timer and close the scrub bracket.
        m_playing = false;
        m_playTimer->stop();
        if (m_playButton) {
            m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        }
        emit scrubEnd();
    }
}

void ViewportTimeline::onPlayTick()
{
    if (!m_playing) return;

    // Advance one frame.  dt spans the full range across the frame
    // count: dt = (t1 - t0) / max(frames-1, 1).  Pacing is strictly
    // timer-driven — we never gate the next tick on frame arrival.
    const unsigned int denom = (m_numFrames > 1) ? (m_numFrames - 1) : 1;
    const double dt = (m_maxT - m_minT) / static_cast<double>(denom);

    double t = m_time + dt;
    if (t > m_maxT) {
        // Wrap back to the start and continue — LOOP until stopped.
        t = m_minT;
    }

    setTimeValue(t);          // position slider/labels, non-emitting
    emit timeChanged(m_time); // reuse the existing scrub wire
}

void ViewportTimeline::stopPlayback()
{
    // Public stop hook for MainWindow (call before a production
    // render).  Routes through the Play button's toggle so the icon,
    // m_playing flag, timer, and scrub-bracket all unwind through the
    // single onPlayToggled path.  No-op when not playing.
    if (m_playing && m_playButton) {
        m_playButton->setChecked(false);
    }
}
