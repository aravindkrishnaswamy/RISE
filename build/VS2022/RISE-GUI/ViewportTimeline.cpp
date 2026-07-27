//////////////////////////////////////////////////////////////////////
//
//  ViewportTimeline.cpp
//
//////////////////////////////////////////////////////////////////////

#include "ViewportTimeline.h"
#include "Theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QSlider>
#include <QToolButton>
#include <QTimer>
#include <QSignalBlocker>
#include <QSize>

#include <algorithm>

ViewportTimeline::ViewportTimeline(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(58);
    setAutoFillBackground(true);
    setObjectName(QStringLiteral("viewportTimeline"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 12, 0);
    layout->setSpacing(10);

    // ---- Transport: rewind / play-stop / to-end ------------------------
    auto* transport = new QWidget(this);
    auto* transportLayout = new QHBoxLayout(transport);
    transportLayout->setContentsMargins(0, 0, 0, 0);
    transportLayout->setSpacing(7);

    // Icon-system upgrade: TimelineSlider.swift:68/89 actually keeps these
    // two as plain Text("⏮")/Text("⏭") glyphs (NOT SF Symbols -- only the
    // play/stop button below uses Image(systemName:)), foregroundColor
    // Theme.textFaint.  On Windows those Miscellaneous Symbols glyphs are
    // a color-emoji fallback risk (Segoe UI Symbol -> Segoe UI Emoji),
    // the same font-fallback problem documented elsewhere in this app --
    // swap for the Lucide "skip-back"/"skip-forward" outline glyphs
    // (closest visual analogue to ⏮/⏭) at the same Theme.textFaint tint
    // Mac already uses here (matching the QSS color this replaces, not
    // the play button's textPrimary -- play/stop is a filled action
    // button, this is a plain transport affordance like Mac's).
    m_rewindButton = new QToolButton(transport);
    m_rewindButton->setIconSize(QSize(14, 14));
    m_rewindButton->setToolTip(tr("Jump to start"));
    m_rewindButton->setStyleSheet(QStringLiteral("QToolButton { border: none; }"));
    transportLayout->addWidget(m_rewindButton);

    // Play/Stop toggle.  Checkable — checked = playing.  Bundled Lucide
    // "play-filled"/"square-filled" glyphs (Theme::icon), swapped in
    // onPlayToggled, mirror TimelineSlider.swift:76's SF
    // play.fill/stop.fill (filled, not the outline "play"/"square").
    m_playButton = new QToolButton(transport);
    m_playButton->setCheckable(true);
    m_playButton->setFixedSize(24, 24);
    m_playButton->setIconSize(QSize(14, 14));
    m_playButton->setToolTip("Play the active animation (loops until stopped)");
    transportLayout->addWidget(m_playButton);

    m_toEndButton = new QToolButton(transport);
    m_toEndButton->setIconSize(QSize(14, 14));
    m_toEndButton->setToolTip(tr("Jump to end"));
    m_toEndButton->setStyleSheet(QStringLiteral("QToolButton { border: none; }"));
    transportLayout->addWidget(m_toEndButton);

    layout->addWidget(transport);

    // "MM:SS / MM:SS" -- two adjacent labels (current, dim "/ max")
    // rather than a single interpolated string, so setRange()/
    // updateLabels() keep independently updating each half exactly as
    // before; only the paint (mono font, dim max) and position (both
    // before the track, matching the comp) changed.
    m_currentLabel = new QLabel(this);
    m_currentLabel->setFont(Theme::mono(11));
    m_currentLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(m_currentLabel);

    m_maxLabel = new QLabel(this);
    m_maxLabel->setFont(Theme::mono(11));
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
    layout->addWidget(m_slider, 1);

    m_renderMovieBtn = new QToolButton(this);
    m_renderMovieBtn->setText(tr("Render movie\xE2\x80\xA6"));
    m_renderMovieBtn->setFont(Theme::sans(11));
    m_renderMovieBtn->setCursor(Qt::PointingHandCursor);
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

    // LIVE THEME-SWITCH CONTRACT (Theme.h): applies every token-dependent
    // stylesheet/icon/palette site above from the CURRENT Theme:: values.
    m_themeReady = true;
    restyleTheme();
}

void ViewportTimeline::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_themeReady && m_themeEpochSeen != Theme::paletteEpoch()) {
        restyleTheme();
    }
}

void ViewportTimeline::restyleTheme()
{
    // Reapplies every one of this widget's own token-dependent styling
    // sites from the CURRENT Theme:: token values. Called once at the
    // end of the constructor and again from changeEvent() on every
    // QEvent::PaletteChange. Idempotent, creates no widgets. See Theme.h's
    // LIVE THEME-SWITCH CONTRACT (MainWindow::restyleTheme is the
    // reference implementation this mirrors).
    m_themeEpochSeen = Theme::paletteEpoch();

    // setPalette() is an explicit per-widget override, so it does NOT
    // automatically track QApplication::setPalette() -- has to be
    // re-applied here from the current token.
    {
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::bgTimeline);
        setPalette(pal);
    }

    setStyleSheet(QStringLiteral("#viewportTimeline { border-top: 1px solid %1; }")
        .arg(Theme::hex(Theme::borderHairline)));

    if (m_rewindButton) m_rewindButton->setIcon(Theme::icon(QStringLiteral("skip-back"), 14, Theme::textFaint));
    if (m_toEndButton) m_toEndButton->setIcon(Theme::icon(QStringLiteral("skip-forward"), 14, Theme::textFaint));

    if (m_playButton) {
        // Icon depends on live playback state, not just the theme --
        // re-derive from m_playing rather than assuming "play-filled"
        // (this is also called mid-playback on a live theme switch).
        m_playButton->setIcon(Theme::icon(
            m_playing ? QStringLiteral("square-filled") : QStringLiteral("play-filled"),
            14, Theme::textPrimary));
        // Token-audit (whiteAlpha -> semantic fill token): this used to
        // hardcode Theme::whiteAlpha(0.1)/(0.2) directly, which stays
        // white-tinted even in light mode. Theme::fillHover/fillActive
        // are the same alpha family already flipped to black-opacity in
        // LightPalette (see Theme.cpp) -- "Active" also reads naturally
        // as "the button IS active/checked", matching the QSS selector
        // below it drives. Mirrors TimelineSlider.swift:80's
        // `Theme.fillActive` background for this same button (Mac uses
        // ONE constant fill regardless of playing state; Windows keeps
        // the existing two-state distinction -- fillHover baseline,
        // fillActive when checked -- rather than flattening it, since
        // that visual distinction predates this theme-switch conversion
        // and isn't this pass's concern).
        m_playButton->setStyleSheet(QStringLiteral(
            "QToolButton { background-color: %1; border-radius: 6px; color: %2; }"
            "QToolButton:checked { background-color: %3; }")
            .arg(Theme::rgba(Theme::fillHover), Theme::hex(Theme::textPrimary), Theme::rgba(Theme::fillActive)));
    }

    if (m_currentLabel) m_currentLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textSecondary)));
    if (m_maxLabel) m_maxLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));

    if (m_slider) {
        // Token-audit (whiteAlpha -> semantic fill token): this used to
        // hardcode Theme::whiteAlpha(0.12) for the groove background,
        // which stays white-tinted even in light mode. Theme::fillTrough
        // is the mode-aware token for exactly this "recessed track"
        // surface -- mirrors TimelineSlider.swift:109's
        // `Theme.fillTrough` fill on the same scrub-track groove.
        m_slider->setStyleSheet(QStringLiteral(
            "QSlider::groove:horizontal { height: 2px; background: %1; border-radius: 1px; }"
            "QSlider::sub-page:horizontal { height: 2px; background: %2; border-radius: 1px; }"
            "QSlider::handle:horizontal { width: 2px; margin: -6px 0; background: %3; border-radius: 1px; }")
            .arg(Theme::rgba(Theme::fillTrough), Theme::hex(Theme::accent), Theme::hex(Theme::textPrimary)));
    }

    if (m_renderMovieBtn) {
        m_renderMovieBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: 1px solid %2; border-radius: 6px; padding: 5px 11px; }"
            "QToolButton:disabled { color: %3; border-color: %4; }")
            .arg(Theme::hex(Theme::textTertiary), Theme::hex(Theme::borderLight),
                 Theme::hex(Theme::textDisabled), Theme::hex(Theme::borderHairline)));
    }
}

void ViewportTimeline::setRange(double minT, double maxT, double canonicalTime)
{
    // Keep the user's Begin/Move/End composite intact.  Applying an
    // out-of-range update now would call jumpToTime(), whose nested Begin
    // deliberately replaces the open composite; hiding/rebuilding the
    // pressed slider can likewise prevent sliderReleased from arriving.
    if (m_scrubbing) {
        m_pendingMinT = minT;
        m_pendingMaxT = maxT;
        m_pendingCanonicalTime = canonicalTime;
        m_hasPendingRange = true;
        return;
    }

    const bool rangeChanged = m_minT != minT || m_maxT != maxT;
    const bool timeChanged = m_time != canonicalTime;
    if (!rangeChanged && !timeChanged) return;
    stopPlayback();
    m_minT = minT;
    m_maxT = maxT;
    const double clampedTime = std::clamp(canonicalTime, m_minT, m_maxT);
    if (clampedTime != canonicalTime) {
        jumpToTime(clampedTime);
    } else {
        // Undo/Redo can change canonical time without touching this widget;
        // also reproject when only the range changed.
        setTimeValue(canonicalTime);
    }
}

void ViewportTimeline::setAnimationFrameCount(unsigned int numFrames)
{
    if (m_numFrames == numFrames) return;
    stopPlayback();
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
    applyPendingRange();
}

void ViewportTimeline::onSliderMoved(int sliderValue)
{
    const double frac = sliderValue / static_cast<double>(m_slider->maximum());
    m_time = m_minT + frac * (m_maxT - m_minT);
    // A movement after setRange deferred its snapshot is the newer
    // canonical user intent; preserve it when that range is applied on
    // release instead of snapping back to the poll's earlier time.
    if (m_hasPendingRange) m_pendingCanonicalTime = m_time;
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
            m_playButton->setIcon(Theme::icon(QStringLiteral("square-filled"), 14, Theme::textPrimary));
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
            m_playButton->setIcon(Theme::icon(QStringLiteral("play-filled"), 14, Theme::textPrimary));
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

void ViewportTimeline::finalizeOpenTimelineInteraction()
{
    stopPlayback();
    // A hide/disable can prevent the physical mouse-release event.  Reset
    // QAbstractSlider's own pressed state as well as our wrapper flag, or
    // the next press after re-show may not emit sliderPressed.  Block the
    // synthetic sliderReleased because we close the controller bracket once
    // through m_scrubbing below.
    if (m_slider && m_slider->isSliderDown()) {
        QSignalBlocker blocker(m_slider);
        m_slider->setSliderDown(false);
    }
    if (m_scrubbing) {
        m_scrubbing = false;
        emit scrubEnd();
    }
    m_hasPendingRange = false;
}

void ViewportTimeline::applyPendingRange()
{
    if (!m_hasPendingRange) return;
    const double minT = m_pendingMinT;
    const double maxT = m_pendingMaxT;
    const double canonicalTime = m_pendingCanonicalTime;
    m_hasPendingRange = false;
    setRange(minT, maxT, canonicalTime);
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
