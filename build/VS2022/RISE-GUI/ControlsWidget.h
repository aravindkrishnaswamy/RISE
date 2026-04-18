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

#include "RenderEngine.h"

class ControlsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ControlsWidget(QWidget* parent = nullptr);

    void setRenderState(RenderEngine::State state);
    void setHasAnimation(bool hasAnimation);
    void setHasScene(bool hasScene);

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

private:
    void updateButtonStates();

    QPushButton* m_openBtn = nullptr;
    QPushButton* m_editBtn = nullptr;
    QPushButton* m_clearBtn = nullptr;
    QPushButton* m_renderBtn = nullptr;
    QPushButton* m_renderAnimBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;

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

    RenderEngine::State m_state = RenderEngine::Idle;
    bool m_hasAnimation = false;
    bool m_hasScene = false;
};

#endif // CONTROLSWIDGET_H
