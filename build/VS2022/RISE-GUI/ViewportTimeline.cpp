//////////////////////////////////////////////////////////////////////
//
//  ViewportTimeline.cpp
//
//////////////////////////////////////////////////////////////////////

#include "ViewportTimeline.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QSlider>
#include <QToolButton>
#include <QTimer>
#include <QStyle>
#include <QSignalBlocker>

#include <algorithm>

ViewportTimeline::ViewportTimeline(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(58);
    setAutoFillBackground(true);
    {
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::bgTimeline);
        setPalette(pal);
    }
    setObjectName(QStringLiteral("viewportTimeline"));
    setStyleSheet(QStringLiteral("#viewportTimeline { border-top: 1px solid %1; }")
        .arg(Theme::hex(Theme::borderHairline)));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 12, 0);
    layout->setSpacing(10);

    // ---- Transport: rewind / play-stop / to-end ------------------------
    auto* transport = new QWidget(this);
    auto* transportLayout = new QHBoxLayout(transport);
    transportLayout->setContentsMargins(0, 0, 0, 0);
    transportLayout->setSpacing(7);

    m_rewindButton = new QToolButton(transport);
    m_rewindButton->setText(QString::fromUtf8("\xE2\x8F\xAE"));   // ⏮
    m_rewindButton->setToolTip(tr("Jump to start"));
    m_rewindButton->setStyleSheet(QStringLiteral("QToolButton { color: %1; border: none; }")
        .arg(Theme::hex(Theme::textFaint)));
    transportLayout->addWidget(m_rewindButton);

    // Play/Stop toggle.  Checkable — checked = playing.  Uses the
    // platform style's media-play icon so the affordance reads as a
    // transport control rather than a generic button.
    m_playButton = new QToolButton(transport);
    m_playButton->setCheckable(true);
    m_playButton->setFixedSize(24, 24);
    m_playButton->setToolTip("Play the active animation (loops until stopped)");
    m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_playButton->setStyleSheet(QStringLiteral(
        "QToolButton { background-color: %1; border-radius: 6px; color: %2; }"
        "QToolButton:checked { background-color: %3; }")
        .arg(Theme::rgba(Theme::whiteAlpha(int(0.1 * 255))), Theme::hex(Theme::textPrimary),
             Theme::rgba(Theme::whiteAlpha(int(0.2 * 255)))));
    transportLayout->addWidget(m_playButton);

    m_toEndButton = new QToolButton(transport);
    m_toEndButton->setText(QString::fromUtf8("\xE2\x8F\xAD"));   // ⏭
    m_toEndButton->setToolTip(tr("Jump to end"));
    m_toEndButton->setStyleSheet(QStringLiteral("QToolButton { color: %1; border: none; }")
        .arg(Theme::hex(Theme::textFaint)));
    transportLayout->addWidget(m_toEndButton);

    layout->addWidget(transport);

    // "MM:SS / MM:SS" -- two adjacent labels (current, dim "/ max")
    // rather than a single interpolated string, so setRange()/
    // updateLabels() keep independently updating each half exactly as
    // before; only the paint (mono font, dim max) and position (both
    // before the track, matching the comp) changed.
    m_currentLabel = new QLabel(this);
    m_currentLabel->setFont(Theme::mono(11));
    m_currentLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textSecondary)));
    m_currentLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(m_currentLabel);

    m_maxLabel = new QLabel(this);
    m_maxLabel->setFont(Theme::mono(11));
    m_maxLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    m_maxLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(m_maxLabel);

    // Thin custom track: QSS-restyled QSlider (trough/fill/2px-wide
    // playhead) rather than a from-scratch reimplementation, so the
    // EXISTING press/moved/released scrub contract (and its undo-
    // bracketing semantics) carries over unchanged -- only the paint
    // is different.
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, 1000);   // virtual ticks; we map to [m_minT, m_maxT]
    m_slider->setValue(0);
    m_slider->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { height: 2px; background: %1; border-radius: 1px; }"
        "QSlider::sub-page:horizontal { height: 2px; background: %2; border-radius: 1px; }"
        "QSlider::handle:horizontal { width: 2px; margin: -6px 0; background: %3; border-radius: 1px; }")
        .arg(Theme::rgba(Theme::whiteAlpha(int(0.12 * 255))), Theme::hex(Theme::accent), Theme::hex(Theme::textPrimary)));
    layout->addWidget(m_slider, 1);

    m_renderMovieBtn = new QToolButton(this);
    m_renderMovieBtn->setText(tr("Render movie\xE2\x80\xA6"));
    m_renderMovieBtn->setFont(Theme::sans(11));
    m_renderMovieBtn->setCursor(Qt::PointingHandCursor);
    m_renderMovieBtn->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; border: 1px solid %2; border-radius: 6px; padding: 5px 11px; }"
        "QToolButton:disabled { color: %3; border-color: %4; }")
        .arg(Theme::hex(Theme::textTertiary), Theme::hex(Theme::borderLight),
             Theme::hex(Theme::textDisabled), Theme::hex(Theme::borderHairline)));
    connect(m_renderMovieBtn, &QToolButton::clicked, this, &ViewportTimeline::renderMovieClicked);
    layout->addWidget(m_renderMovieBtn);

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
    connect(m_rewindButton, &QToolButton::clicked, this, &ViewportTimeline::onRewindClicked);
    connect(m_toEndButton,  &QToolButton::clicked, this, &ViewportTimeline::onToEndClicked);

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

QString ViewportTimeline::formatTime(double seconds)
{
    const int totalSeconds = static_cast<int>(std::max(0.0, seconds) + 0.5);
    const int mm = totalSeconds / 60;
    const int ss = totalSeconds % 60;
    return QStringLiteral("%1:%2")
        .arg(mm, 2, 10, QLatin1Char('0'))
        .arg(ss, 2, 10, QLatin1Char('0'));
}

void ViewportTimeline::updateLabels()
{
    m_currentLabel->setText(formatTime(m_time));
    m_maxLabel->setText(QStringLiteral("/ %1").arg(formatTime(m_maxT)));
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

void ViewportTimeline::jumpToTime(double t)
{
    // A running Play interrupts the same way a manual scrub does
    // (routes through the Play button's own toggle, closing ITS scrub
    // bracket cleanly) before this jump opens its own single-step one.
    stopPlayback();
    emit scrubBegin();
    setTimeValue(t);
    emit timeChanged(m_time);
    emit scrubEnd();
}

void ViewportTimeline::onRewindClicked()
{
    jumpToTime(m_minT);
}

void ViewportTimeline::onToEndClicked()
{
    jumpToTime(m_maxT);
}

void ViewportTimeline::setRenderMovieEnabled(bool enabled)
{
    if (m_renderMovieBtn) m_renderMovieBtn->setEnabled(enabled);
}
