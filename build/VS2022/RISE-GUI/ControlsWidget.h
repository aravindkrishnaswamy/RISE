//////////////////////////////////////////////////////////////////////
//
//  ControlsWidget.h - Buttons, progress bar, and elapsed time display.
//
//  Ported from the Mac app's controlsPanel in ContentView.swift.
//
//////////////////////////////////////////////////////////////////////

#ifndef CONTROLSWIDGET_H
#define CONTROLSWIDGET_H

#include <QWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QSlider>
#include <QComboBox>
#include <QStringList>

#include "RenderEngine.h"

// L5e — QSlider subclass that emits `resetRequested` on a double-
// click anywhere in the widget rect.  Used by the Exposure slider:
// users double-click the track to snap back to 0 EV (matches the
// macOS port's TapGesture(count: 2) gesture).  No "Reset" button —
// the gesture is the only affordance, mirroring DCC norms.
class ExposureSlider : public QSlider
{
    Q_OBJECT
public:
    explicit ExposureSlider(QWidget* parent = nullptr) : QSlider(Qt::Horizontal, parent) {}
signals:
    void resetRequested();
protected:
    void mouseDoubleClickEvent(QMouseEvent* /*e*/) override { emit resetRequested(); }
};

class ControlsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ControlsWidget(QWidget* parent = nullptr);

    void setRenderState(RenderEngine::State state);
    void setHasAnimation(bool hasAnimation);
    void setHasScene(bool hasScene);

    // Named animation paths — populate the active-animation dropdown.
    // `names` is the scene's declared animation names; `activeIdx` is
    // the currently-active one (-1 = none).  The whole row is hidden
    // when there are fewer than 2 animations (a single "(default)"
    // animation shouldn't clutter the panel).  Uses QSignalBlocker so
    // programmatic population doesn't echo back through animationSelected.
    void setAnimationNames(const QStringList& names, int activeIdx);

    // L5e — Sync slider with engine's current EV (e.g. on scene
    // load / external programmatic change).
    void setExposureEV(double ev);

    // L5e round-2 — Grey out the exposure slider while HDR Preview
    // is on.  The HDR display path uses ForHDRDisplay (no tone
    // curve, OS-compositor-driven dynamic-range mapping); applying
    // our own exposure on top double-maps the radiance signal,
    // visible as flicker / hue shifts on HDR-capable monitors.
    // Same gating rule as the View > Tone Curve menu.
    void setHDREnabled(bool hdrOn);

public slots:
    void updateProgress(double fraction, const QString& title);
    void updateElapsedTime(double seconds);
    void updateRemainingTime(double seconds, bool hasEstimate);

signals:
    void openSceneClicked();
    void editClicked();
    void clearClicked();
    void renderClicked();
    void renderAnimationClicked();
    void cancelClicked();

    // L5e — User dragged or double-clicked the exposure slider.
    // MainWindow forwards to RenderEngine::setViewExposureEV.
    void exposureChanged(double ev);

    // User picked a different active animation from the dropdown.
    // MainWindow forwards to ViewportBridge::setSelectedAnimation +
    // re-reads the timeline range and re-scrubs the preview.
    void animationSelected(int idx);

private:
    void updateButtonStates();

    // Slot for the animation-combo's currentIndexChanged(int) — emits
    // animationSelected.  Suppressed during programmatic population by
    // the QSignalBlocker in setAnimationNames.
    void onAnimComboChanged(int index);

    QPushButton* m_openBtn = nullptr;
    QPushButton* m_editBtn = nullptr;
    QPushButton* m_clearBtn = nullptr;
    QPushButton* m_renderBtn = nullptr;
    QPushButton* m_renderAnimBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;

    // Named-animation picker.  m_animRow is the labeled-row container
    // (label + combo); hidden/shown as a unit when the scene has <2
    // animations, matching how other optional rows hide.
    QWidget*    m_animRow = nullptr;
    QComboBox*  m_animCombo = nullptr;

    QProgressBar* m_progressBar = nullptr;
    QLabel* m_progressTitle = nullptr;
    QLabel* m_progressPercent = nullptr;
    QLabel* m_elapsedLabel = nullptr;
    QLabel* m_remainingLabel = nullptr;
    QLabel* m_cancellingLabel = nullptr;
    double m_lastElapsed = 0.0;
    double m_lastRemaining = 0.0;
    bool m_haveRemainingEstimate = false;
    void refreshTimeLabels();

    QWidget* m_progressGroup = nullptr;

    // L5e — Exposure slider.  Slider value is integer (Qt
    // limitation — QSlider is int-only); we map [-60, +60] → EV
    // in 0.1-stop increments.  The label next to the slider
    // displays the float value with sign + 1-decimal precision
    // ("0.0 EV", "+1.5 EV", "-3.2 EV").
    static constexpr int kExposureSliderMin =  -60;  //  -6.0 EV
    static constexpr int kExposureSliderMax =   60;  //  +6.0 EV
    ExposureSlider* m_exposureSlider = nullptr;
    QLabel*         m_exposureLabel  = nullptr;
    void onExposureSliderChanged(int value);
    void onExposureResetRequested();

    RenderEngine::State m_state = RenderEngine::Idle;
    bool m_hasAnimation = false;
    bool m_hasScene = false;
};

#endif // CONTROLSWIDGET_H
